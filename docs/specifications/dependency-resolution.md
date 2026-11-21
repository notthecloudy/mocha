# Dependency Resolution

Status: implemented in `native/src/loader/semver.cpp` and
`native/src/loader/plan.cpp`; unit-tested; exercised by the example mods.

This document defines the version grammar, the constraint grammar, and the exact
resolution algorithm. It is a specification of the code as written, including
its deliberate omissions.

## 1. Mental model

Resolution is **not** a package manager. There is no registry, no download, no
version selection among candidates, and no lockfile. The input is the set of
manifests physically present in `mods/`. For each `id` there is either exactly
one enabled mod or none, and then the graph is ordered.

That rules out a whole class of behavior by design:

- You cannot have two versions of the same mod installed. Duplicate ids are
  disabled, both of them.
- A constraint never causes a different version to be chosen. It only checks the
  version that is present and disables the dependent if it does not match.
- Optional dependencies do not pull anything in. They only skip the check.

## 2. Version grammar

```
version    = number [ "." number [ "." number ] ] [ "-" prerelease ] [ "+" build ]
number     = 1*DIGIT          value <= 1,000,000
prerelease = 1*( any character except NUL )   (non-empty; split on ".")
build      = any characters    (parsed and discarded)
```

Details that are observable:

- Leading and trailing whitespace is trimmed.
- `+build` is removed first, then everything after the first `-` is the
  prerelease. A trailing `-` with nothing after it is invalid.
- At most three numeric components. A fourth `.` makes the version invalid.
- Missing components are zero: `1` = `1.0.0`, `1.2` = `1.2.0`.
- Each numeric component is capped at `1,000,000`; a longer run of digits is
  invalid rather than truncated.
- The prerelease character set is **not** validated (only non-emptiness). This
  is looser than SemVer 2.0.0 and is stated here so nobody assumes otherwise.

Comparison (`compare_versions`):

1. major, then minor, then patch numerically.
2. Prerelease: split on `.` and compare segment by segment; if both segments are
   all digits, compare numerically; a numeric segment sorts **before** an
   alphanumeric one; otherwise compare bytewise. If all common segments are
   equal, the version with more segments sorts higher.
3. No prerelease sorts **after** any prerelease of the same numeric version.

## 3. Constraint grammar

A constraint is one or more **alternatives** separated by `||`. An alternative
is one or more **comparators** separated by whitespace or commas (commas are
rewritten to spaces). A version satisfies the constraint if it satisfies every
comparator of at least one alternative.

```
constraint  = alternative *( "||" alternative )
alternative = comparator *( ( space | "," ) comparator )
comparator  = [ ">=" | "<=" | ">" | "<" | "=" | "^" | "~" ] version
            | wildcard
wildcard    = ("*" | "x" | "X")                        ; alone: matches anything
            | number "." ("*"|"x"|"X")
            | number "." number "." ("*"|"x"|"X")
```

Special cases:

- `""` (empty or whitespace) and `"*"` mean "any version" and short-circuit to
  true.
- Inside an alternative, a bare `*`/`x`/`X` token contributes no constraint but
  counts as having seen one, so `"x"` matches anything and `">=1.0 x"` behaves
  like `">=1.0"`. An alternative with no tokens at all (e.g. from `"||"`) does
  not match.
- Without an operator, a token containing `x`, `X` or `*` is parsed as a
  wildcard. The wildcard must be the trailing component; `1.x.2` is an error.
- `=` is accepted and is equivalent to no operator (exact match).

### Comparator semantics

| Form | Meaning |
|---|---|
| `1.2.3` or `=1.2.3` | exactly `1.2.3` |
| `>1.2.3` / `>=1.2.3` | strictly greater / greater or equal |
| `<1.2.3` / `<=1.2.3` | strictly less / less or equal |
| `^1.2.3` | `>=1.2.3` and `<2.0.0` |
| `^0.2.3` | `>=0.2.3` and `<0.3.0` |
| `^0.0.3` | `>=0.0.3` and `<0.0.4` |
| `~1.2.3` | `>=1.2.3` and `<1.3.0` |
| `1.x` | `>=1.0.0` and `<2.0.0` |
| `1.2.x` | `>=1.2.0` and `<1.3.0` |
| `*` | every valid version |
| `>=1.0 <2.0` | both comparators (AND) |
| `<1.0 || >=2.0` | either alternative (OR) |

Notes:

- Caret's upper bound is chosen from the first non-zero component, as above.
- Tilde always increments the minor, even when it was not written. `~1` is
  parsed as `1.0.0`, so it means `>=1.0.0 <1.1.0`. This differs from npm's
  `~1` (`<2.0.0`); the implementation rule is the one documented here.
- Prerelease versions only satisfy a comparator if they compare in range. There
  is no npm-style rule that a prerelease is excluded unless the comparator
  mentions one; a prerelease can satisfy `>=1.0.0` on its own merits.

## 4. Resolution algorithm

`resolve_mods(std::vector<Manifest>)` performs these steps, in this order.

### Step 1 — deterministic sort

Sort by `(id, manifest_path)`. This fixes the order in which duplicates and
errors are reported and removes reliance on directory enumeration order.

### Step 2 — version validity

For each manifest, `parse_version`. Failure sets
`reason = "invalid version '<version>'"` and leaves it disabled. Success enables
it.

### Step 3 — duplicate ids

Count ids. Any id with count > 1 disables **all** entries with that id:

```
duplicate mod id
```

`index` maps each id to the *last* sorted entry. A dependency on a duplicated id
therefore sees a disabled target and is handled by step 4.

### Step 4 — dependency disabling (fixpoint)

Repeat until no change (bounded by `mods.size() + 2` for safety). For each
enabled mod, for each dependency:

| Situation | Required dependency | Optional dependency |
|---|---|---|
| target id absent | disable mod: `missing dependency '<id>'` | ignore |
| target present but disabled | disable mod: `dependency '<id>' could not be loaded` | ignore |
| target enabled, version fails | disable mod: `dependency '<id>' version <v> does not satisfy '<constraint>'` | ignore |
| target enabled, version passes | keep | keep |

The first failure stops checking that mod, so its `reason` is the first problem
in manifest order. Disabling can cascade (a disabled dependency disables its
dependents), which is why this is a fixpoint loop.

A constraint that fails to *parse* is treated as unsatisfied; the surfaced
reason is the `does not satisfy` message, not the parse error. Constraint syntax
errors are therefore reported as version mismatch.

### Step 5 — topological order

Only enabled mods participate.

1. Build edges `dependency → dependent` for every dependency whose target is
   also enabled. Optional dependencies still create an edge when their target is
   present; optional only relaxes the *failure* rules, not ordering.
2. Kahn's algorithm, but the ready set is scanned in sorted order and the
   **lexicographically smallest ready id** is emitted first. So among
   independent mods, order is by id, not by manifest file order.
3. If no ready node remains and nodes are pending, a cycle exists.

### Step 6 — cycle handling

On a cycle:

1. Every still-pending mod is disabled with `dependency cycle`.
   "Pending" includes the cycle members *and* every mod downstream of them,
   because none of them could be emitted.
2. Re-scan enabled mods: any non-optional dependency that is no longer enabled
   disables the dependent with `dependency '<id>' could not be loaded`.
3. Restart step 5 from scratch. This repeats until either a complete order is
   produced or a pass disables nothing (at which point it stops).

Reporting only distinguishes "in a cycle or blocked by one" from "depends on
something that was disabled"; individual cycle edges are not named. This is a
deliberate simplification: the set of disabled ids and the reason are enough to
fix the manifests.

### Step 7 — results

- `errors` contains `"<id>: <reason>"` for every disabled mod, in sorted order.
- `load_order` is the enabled mods in the computed order.

## 5. Worked examples

These are the shapes used by the unit tests and the example mods.

**Satisfied chain**

```
hello     1.0.0
dependent 1.0.0  depends on hello ^1.0.0
```

Both enabled. Order: `dependent` has indegree 1, `hello` is ready first, then
`dependent`. Load order `[hello, dependent]`.

**Missing dependency**

```
missing-dep 1.0.0 depends on mocha.example.absent `*`
```

`mocha.example.absent` is absent → disabled with
`missing dependency 'mocha.example.absent'`. This is the artifact's
`skipped_mod` line.

**Unsatisfied constraint**

```
core 2.1.0
user 1.0.0 depends on core ^1.0.0
```

`user` disabled with
`dependency 'core' version 2.1.0 does not satisfy '^1.0.0'`.

**Optional dependency**

```
user 1.0.0 depends on extras ^1.0.0 (optional: true)
```

If `extras` is absent or disabled, `user` still loads. If `extras` is present
and enabled, ordering still puts `extras` first; if it is present but the
version does not match, the optional dependency is ignored (no failure).

**Cycle**

```
a 1.0.0 depends on b
b 1.0.0 depends on a
c 1.0.0 depends on a
d 1.0.0 depends on c
```

Kahn emits nothing. `a`, `b`, `c` and `d` are all still pending — `d` is
downstream of the cycle and can never become ready — so all four are disabled
with `dependency cycle`. The distinction this produces is "blocked by a cycle"
versus "depends on something disabled for another reason", which is why the
message does not name individual edges.

The re-scan after cycle disabling is defensive: it catches an enabled mod whose
non-optional dependency was just disabled. In the current graph construction an
already-emitted mod cannot depend on a pending one, so in practice the pending
set is what gets disabled.

**Duplicate**

```
hello 1.0.0   (hello.json)
hello 0.9.0   (hello-old.json)
```

Both disabled with `duplicate mod id`, regardless of which version is newer.

## 6. Determinism

Given the same directory contents:

- the sort key is total (`id`, then path);
- the disabling fixpoint visits mods in that order;
- the ready-set choice is the smallest id;
- cycle handling restarts from a fresh pass with deterministic input.

The same input therefore always produces the same `load_order`, the same
`errors`, and the same summary. Filesystem enumeration order is never consulted
after the initial `readdir`, because `discover_mods` sorts paths before parsing.

## 7. Limits and non-claims

- No version solving, no backtracking, no "pick the highest satisfying version".
- No transitive visibility: a mod sees only its own direct dependencies in the
  manifest. Any mod can still `dlopen` anything at runtime; the loader does not
  enforce link boundaries.
- No conflict declarations beyond id + version.
- Names are not unique; only ids are. Two different ids may share a `name`.
- Constraint syntax that fails to parse is reported as a version mismatch, not
  as a syntax error. Check the log line for the mod version and constraint text.
- Only the grammar and algorithms above are covered by unit tests; no claim is
  made about compatibility with npm, Cargo, or any other resolver's edge cases.
