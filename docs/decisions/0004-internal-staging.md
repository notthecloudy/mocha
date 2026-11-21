# ADR 0004: Stage mods under internal storage before `dlopen`

- Status: accepted, implemented
- Date: 2026-11-19
- Code: `native/src/loader/loader.cpp:228-262`,
  `native/src/loader/paths.cpp:118-211`
- Related: ADR 0001, `docs/specifications/mod-manifest.md`

## Context

Mods are discovered in a user-visible location:
`<external files>/mocha/mods/`, which on the test device is
`/storage/emulated/0/Android/data/dev.mocha.testapp/files/mocha/mods/`. The
manifest that describes a mod lives there and its `library` path points at a
sibling `.so`.

Android's dynamic linker does not allow arbitrary paths. Since Android 7,
native libraries are loaded through a per-app **linker namespace** created by
`nativeloader`, with an allow-list of `permitted_paths`. Code that was not
loaded from a permitted path is refused with a `linker` warning of the form:

```
W linker: library "/storage/emulated/0/Android/data/dev.mocha.testapp/files/…/hello-mod.so"
  … needed or dlopened by "…/lib/arm64/libmocha.so" is not accessible for the namespace:
  [name="clns-9", … permitted_paths="/data:/mnt/expand:/data/data/dev.mocha.testapp"]
```

That is exactly what the first end-to-end run produced: every mod discovered
under external storage failed the direct `dlopen`, and `permitted_paths`
listed only `/data`, `/mnt/expand`, `/data/data/dev.mocha.testapp`.

The loader has no root, cannot join another namespace, and should not ask the
user to place code in a system directory. It also should not require mods to
be installed as APK libraries.

## Decision

Keep the user-facing mods directory on external storage, but **load code from
app-internal storage**:

1. Try `dlopen(<manifest.library_path>, RTLD_NOW | RTLD_LOCAL)` first. Some
   devices/configurations may permit it (for example if the path is already
   inside the app's private directory), and a direct load avoids a copy.
2. If that fails, capture `dlerror()`, then copy the library to
   `<internal files root>/mocha/native/<stem>.so`, where the internal root is
   `getFilesDir()` and `/data/data/<pkg>` (or `/data/user/0/<pkg>`) is inside
   the namespace's permitted paths.
3. Copy with `overwrite_existing`, `chmod 0755`, and retry `dlopen` on the
   internal path. Log `mod <id> loaded from internal copy`.
4. If the copy or the second `dlopen` fails, report both errors:
   `dlopen failed (<original>); copying to internal storage failed: <errno>`
   or `dlopen failed (<original>); internal copy failed: <dlerror>`, and mark
   the mod `failed:dlopen`.

The internal copy uses the library's file stem, matching the "sibling `.so`
with the same stem" rule of the manifest, so two mods with the same stem in
different directories would collide. Schema 1 requires mod ids to be unique
but does not constrain file names; this is an accepted limitation (a mod id
is not used in the filename). Overwriting means an updated external library is
picked up on the next launch; there is no checksum cache to invalidate.

The mods **manifest** is still read from external storage directly. JSON is
ordinary data, not code, and file reads are not restricted by the namespace.

## Consequences

Positive:

- Works on stock Android without root, without modifying system policy, and
  without asking the user to install mods into the APK.
- The discovery location stays user-accessible, so a file manager or `adb
  push` can add and update mods.
- Failure is explicit and includes the original `dlerror`, so a namespace
  denial is distinguishable from a missing dependency or wrong ABI in the log.

Negative and risks:

- **Code execution from shared storage.** Anything that can write to the app's
  external files directory can replace a `.so` that the app then loads with
  the app's privileges. On Android 11+ other apps cannot write another app's
  `Android/data` directory through SAF, but `adb`, root, or a compromised
  device can. Mocha does no signature or integrity verification. This is
  documented in `docs/limitations.md`; users must treat the mods directory as
  trusted input.
- **Storage duplication.** Each mod is copied once per launch attempt at its
  declared path; a large mod is stored twice (external original + internal
  copy). The patched test mods are tens of KB; this is not optimized.
- **Stem collision.** Two mods whose libraries share a stem overwrite each
  other's staged copy. The second `dlopen` in the same process may also be
  served from the loader's already-mapped soname cache, so the behavior is not
  simply "last file wins"; it is undefined for that pair. Do not ship two mods
  with the same `.so` name.
- **Internal copy is not cleaned up** on mod removal or update failure. A
  stale `.so` remains in `files/mocha/native`; it is not loaded unless a
  manifest points at a library with that stem.
- Copying a file that changes concurrently can produce a truncated or mixed
  library. The loader does not lock or checksum; `dlopen` may fail, which is
  reported as `failed:dlopen`.
- The copy is a plain byte copy; `chmod 0755` is best-effort and its result is
  not checked. `dlopen` of a non-executable file still succeeds on Android
  (mapping does not require the execute bit), so this is not relied on for
  correctness.

## Alternatives considered

- **Put the mods directory under internal storage** (`getFilesDir()/mocha/
  mods`): avoids the copy, but the user cannot conveniently add mods without
  `run-as` or a debuggable build. Rejected for usability, though the loader
  already falls back to internal storage when external is not writable
  (`paths.cpp`).
- **Rewrite `permitted_paths` / use `android_dlopen_ext` with a namespace**:
  requires platform-private APIs and the app namespace is not extensible
  without root or a system app. Rejected.
- **Load from the APK's `lib/<abi>/`**: would require rebuilding and
  re-signing the APK for every mod, defeating the point of a runtime loader.
  Rejected.
- **Copy everything eagerly at startup, before discovery**: the copy cost is
  paid even for mods that fail dependency resolution or are skipped, and a
  failed copy would need its own error path before the per-mod loop. Lazy
  copy on the first `dlopen` failure keeps the logic in one place.
- **Use `memfd_create` and `/proc/self/fd/N`**: avoids a file on internal
  storage, but `/proc/self/fd` is not in the namespace's permitted paths
  either, and the implementation is more complex. Not attempted.

## Evidence

- `native/src/loader/loader.cpp:228-262` (direct attempt, original
  `dlerror`, internal staging, overwrite, `chmod`, retry, both error forms).
- `native/src/loader/paths.cpp:118-211` (external root preferred when
  writable, internal root fallback, `<root>/mocha` creation; the staging code
  uses the internal root regardless of which root discovery used).
- `artifacts/emulator-20261118-191549/logcat.txt:805-821`: linker denies the
  external path with `permitted_paths="/data:/mnt/expand:/data/data/
  dev.mocha.testapp"` for `failing-mod.so`, `hello-mod.so`, and
  `dependent-mod.so`; each `Mocha` line immediately after says the mod was
  "loaded from internal copy". The `failing` mod's init failure and the
  summary's `failed=1` are separate (its entry returns non-zero by design).
- `loader-summary.txt` in the same artifact: `loaded=2 failed=1 skipped=1`,
  showing that staging worked for the three libraries that were reached.

## Non-claims

- Verified on the API 37 x86_64 emulator with an app installed at
  `/data/app/...` and external storage at `/storage/emulated/0`. Paths and
  SELinux policy on Quest were unverified at the time of this ADR.
- No claim that direct `dlopen` from external storage ever succeeds on a
  modern Android device; the first attempt is a compatibility fallback, not a
  supported configuration.
- No integrity or provenance guarantee for staged code.

Update (2026-11-20): staging was exercised on a physical Meta Quest 3
(Android 14, 4 KiB pages, non-root). The linker namespace `clns-6` permitted
only `/data:/mnt/expand:/data/data/dev.mocha.testapp`, denied direct `dlopen`
of the external mod libraries, and the loader loaded all three reached mods
from internal copies. This closes the Quest linker/namespace non-claim for
that device and OS; see `docs/testing/results-quest-2026-11-20.md`.
