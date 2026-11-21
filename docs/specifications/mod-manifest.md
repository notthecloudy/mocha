# Mod Manifest Specification (schema 1)

Status: implemented, unit-tested, and exercised on the Android/IL2CPP test app.
Implementations: `native/src/loader/manifest.cpp` (parse/validate),
`native/src/loader/plan.cpp` (resolution), `native/src/loader/semver.cpp`
(version grammar).

## 1. Location and naming

- Manifests live in `<app files>/mocha/mods/`.
- Only files ending in `.json` are considered.
- Files are sorted lexicographically by full path before parsing, so discovery
  order is deterministic.
- A manifest's library is the file with the same stem and a `.so` suffix, in the
  same directory. `hello.json` → `hello.so`.
- A manifest whose library is absent is rejected during discovery and does not
  count as discovered.

The directory is created if missing. Subdirectories are not traversed.

## 2. Document shape

A manifest is a JSON object. Unknown fields are ignored (forward compatibility).
This is the full surface:

```json
{
  "schema": 1,
  "id": "mocha.example.hello",
  "name": "Hello Mod",
  "version": "1.2.0",
  "entry": "mocha_mod_init",
  "shutdown": "mocha_mod_shutdown",
  "abi": 1,
  "dependencies": [
    { "id": "mocha.example.core", "version": ">=1.0.0 <2.0.0" },
    { "id": "mocha.example.optional", "version": "^1.1.0", "optional": true }
  ]
}
```

## 3. Fields

| Field | Type | Required | Default | Rules |
|---|---|---|---|---|
| `schema` | integer | No | `1` | Must be exactly `1` if present. Any other value is a hard reject. |
| `id` | string | **Yes** | — | Regex `[a-z0-9][a-z0-9._-]{0,127}`. 1–128 bytes. Lowercase ASCII letters, digits, `.`, `_`, `-`; first char must be a lowercase letter or digit. |
| `name` | string | No | `id` | Display only. Not used for ordering or identity. |
| `version` | string | **Yes** | — | Must parse as a semver `MAJOR[.MINOR[.PATCH]][-PRERELEASE][+BUILD]`. An unparsable version disables the mod with `invalid version '<v>'`. |
| `entry` | string | No | `mocha_mod_init` | Symbol resolved with `dlsym` in the mod's `.so`. Empty string means the default. |
| `shutdown` | string | No | `mocha_mod_shutdown` | Symbol called at shutdown if present. Empty string means the default. |
| `abi` | integer | No | `1` | Must equal the loader's `MOCHA_ABI_VERSION` (currently `1`). Mismatch is a hard reject naming both values. |
| `dependencies` | array | No | `[]` | Each item is an object; see below. |

Type violations are hard rejects with a specific message; for example
`field 'id' must be a string`, `field 'schema' must be an integer`,
`field 'dependencies' must be an array`.

### Dependencies

| Field | Type | Required | Default | Rules |
|---|---|---|---|---|
| `id` | string | **Yes** | — | Same id rules as a mod id. |
| `version` | string | No | `"*"` | Semver constraint (see `dependency-resolution.md`). |
| `optional` | boolean | No | `false` | If true, a missing/disabled/unsatisfied target does not disable this mod. |

A dependency `id` that is invalid formats is a hard reject:
`dependency id '<id>' is invalid`. A missing `id` is
`missing required field 'id'`.

## 4. Validation order and outcomes

`parse_manifest` produces one of:

- **accept** — a populated `Manifest`, defaults applied.
- **reject** — an error string, and the mod never enters the plan.

Hard rejects (mod not discovered/loaded):

- Invalid JSON, or a root that is not an object.
- `schema` present and not `1`.
- Missing `id` or `version`.
- Non-string `name`/`entry`/`shutdown`, non-integer `schema`/`abi`, non-array
  `dependencies`, non-object dependency entry, non-boolean `optional`.
- Invalid `id` (including dependency ids).
- `abi != 1`.
- Missing sibling `.so` (discovery-level reject).

Soft failures (mod discovered but not loaded):

- Invalid `version` string.
- Duplicate `id` (all copies disabled).
- Missing, disabled, or version-unsatisfied non-optional dependency.
- Dependency cycle (all mods still blocked at cycle detection are disabled,
  which includes dependents of the cycle).

Soft failures are reported in `skipped_mod=` lines and in `skipped=`; hard
rejects are reported in `rejected=` and excluded from `discovered=`.

## 5. Identity and duplicates

`id` is the identity. Two manifests with the same `id` are not "the same mod
updated"; both are disabled with `duplicate mod id`. There is no precedence by
version or path. This is deliberate: silently choosing one would make the load
set depend on filesystem accident.

`manifest_path` is the tiebreaker only for sorting equal ids before the
duplicate check, so the reported order is stable.

## 6. Version grammar

The accepted version form is deliberately close to SemVer 2.0.0:

```
version   = number [ "." number [ "." number ] ] [ "-" prerelease ] [ "+" build ]
number    = 1*DIGIT, value <= 1,000,000
prerelease= dot-separated identifiers of [0-9A-Za-z-]
build     = ignored
```

- Missing minor/patch default to `0` (so `1.2` is `1.2.0`).
- A numeric prerelease identifier compares numerically and sorts below an
  alphanumeric one; a version with a prerelease sorts below the same version
  without one.
- `+build` is parsed and discarded.
- More than three numeric components, a negative number, or a component over
  1,000,000 makes the version invalid.

Constraints are documented in `dependency-resolution.md`.

## 7. Ordering

Load order is not the file order. `resolve_mods` sorts by `(id, manifest_path)`
first, then performs a topological sort of the dependency graph, repeatedly
choosing the lexicographically smallest id whose dependencies are all already
emitted. This makes the order identical across runs and independent of
directory enumeration.

## 8. Examples in this repository

| File | Purpose | Result in the artifact run |
|---|---|---|
| `hello.json` | valid, no dependencies | `loaded` |
| `dependent.json` | depends on `hello` `^1.0.0` | `loaded` |
| `failing.json` | entry returns non-zero | `failed:init` |
| `missing-dep.json` | depends on a non-existent id | `skipped_mod=... missing dependency` |

## 9. Not part of schema 1

- No signature or hash field. Integrity is a packaging concern, not a manifest
  concern; see `docs/limitations.md`.
- No load-priority or `after`/`before` fields. Order is derived from
  dependencies, not declared.
- No per-mod settings or config block. The manifest is metadata; mods read their
  own configuration.
- No game or package identifiers. Nothing in the schema names a title.

Adding a field in a future schema must either be optional with a safe default or
bump `schema`; schema 1 rejects unknown `schema` values rather than guessing.
