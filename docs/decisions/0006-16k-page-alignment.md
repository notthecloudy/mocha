# ADR 0006: Build and package the loader for 16 KiB pages

- Status: accepted, implemented
- Date: 2026-11-19
- Code: `native/CMakeLists.txt:26-29, 37-40`, `tools/mocha_patch.py`
- Related: ADR 0001, `docs/specifications/apk-patching.md` §2

## Context

Android has begun shipping devices with **16 KiB memory pages** in addition to
the long-standing 4 KiB configuration. An ELF shared library whose `PT_LOAD`
segments are aligned for 4 KiB cannot be mapped correctly on a 16 KiB page
device: the loader requires segment alignment compatible with the page size,
and a 4 KiB-aligned segment is rejected or wastes address space depending on
the kernel and loader path. The same applies to native libraries inside an APK
that Android maps directly from the archive (`extractNativeLibs=false`): the
ZIP entry must be **stored** (uncompressed) and its data offset aligned to the
page size.

The target class is Meta Quest, where 16 KiB page devices are expected, and the
test emulator was intentionally configured with 16 KiB pages. A loader that
works only on 4 KiB devices fails on a 16 KiB target before any Mocha code
runs. The physical Quest 3 used for the later L4 run reports 4 KiB pages; the
16 KiB-aligned build loads there too, so the choice does not exclude the device
that was tested.

Both of Mocha's injected libraries (`libmain.so` shim, `libmocha.so`) are new
binaries; their alignment is entirely under our control. The stock Unity
libraries are whatever the game shipped.

## Decision

Two independent measures, both required:

1. **ELF**: link both libraries with

   ```
   -Wl,-z,max-page-size=16384
   -Wl,-z,common-page-size=16384
   ```

   (`native/CMakeLists.txt` for `mocha` and `mocha_shim`). This makes every
   `PT_LOAD` segment's `p_align` `0x4000`.
2. **ZIP**: the patcher re-stores every `lib/**/*.so` entry uncompressed and
   aligns the archive with `zipalign -P 16 -f 4`. `-P 16` aligns uncompressed
   entries to 16 KiB; the trailing `4` keeps 4-byte alignment for the remaining
   entries. Applied to `libmain.so` and `libmocha.so` as well as every existing
   library, so a build with `extractNativeLibs=false` remains installable.

The patcher verifies that `libmain.so` and `libmocha.so` exist and are stored
after writing, and `zipalign -c -P 16 -v 4` is run as part of the build
procedure. Both injected libraries are checked with `readelf -lW` for
`Align 0x4000`.

Neither measure is conditional on a build flag. The cost is a small amount of
padding; making it conditional would produce a loader that silently fails on
the exact devices it targets.

## Consequences

Positive:

- The injected libraries are loadable on 16 KiB-page Android, including the
  Quest target, and on 4 KiB devices (a larger `p_align` is compatible with a
  smaller page size).
- The APK stays valid for direct-mapping installs, not only
  `extractNativeLibs=true` builds. The test APK happens to set `true`; the
  patched layout does not depend on it.
- The check is cheap and mechanical (`readelf`, `zipalign -c`), so a regression
  is caught at build time, not on device.

Negative and risks:

- **Larger binaries and APK.** `-z common-page-size=16384` raises segment
  granularity and padding; `libmocha.so` is 4,866,280 bytes. This is a few
  tens of KB of padding relative to a 4 KiB build, not a functional cost.
- **Requires a toolchain that accepts the flags.** Verified with NDK r27c
  clang/lld. Older NDKs or non-lld linkers may not support
  `-z max-page-size`; the build would fail loudly rather than produce a bad
  binary.
- **ZIP alignment must be redone on every patch.** Editing the APK with
  another tool after patching can invalidate the alignment and the signature;
  re-run the patcher or `zipalign` and re-sign.
- **The stock Unity libraries still have whatever alignment the game shipped.**
  If a title's own `libunity.so`/`libil2cpp.so` are 4 KiB-aligned, Mocha cannot
  fix that, and the game itself may not run on a 16 KiB device. Mocha's
  libraries are correct independently.
- The APK's `extractNativeLibs` value is not changed. Changing it would alter
  install/runtime behavior for the whole app and is outside the loader's
  remit.

## Alternatives considered

- **Rely on `extractNativeLibs=true` and skip ZIP alignment**: extraction
  writes libraries to a directory and maps from there, so archive alignment
  does not matter. But the patcher would then silently produce an APK that
  breaks for direct-mapping builds, and the manifest is not ours to rely on.
  Rejected; alignment is cheap.
- **Patch only the two injected libraries, leave the rest deflated/aligned as
  found**: a direct-mapping APK requires *all* mapped libraries to be stored
  and aligned; one bad entry can fail the install or load. Re-storing all
  `lib/**/*.so` is uniform and simple. Chosen.
- **`-Wl,-z,max-page-size=65536`** (64 KiB, anticipating future devices):
  larger padding for no current benefit, and 16 KiB is the documented
  requirement. Rejected; can be raised later if a device needs it.
- **Runtime workaround (copy to internal storage before loading)**: cannot help
  `libmain.so`, which the Android runtime loads before any of our code runs,
  and does not fix the mapping of the library itself. Rejected.
- **Ship a 4 KiB and a 16 KiB variant**: doubles the artifacts and the patcher
  would have to guess the device. A single 16 KiB-aligned build is compatible
  with both. Rejected.

## Evidence

- `native/CMakeLists.txt:26-29` (`mocha`) and `:37-40` (`mocha_shim`) contain
  the four link options, including both page-size flags.
- `readelf -lW build/android/native/libmocha.so`:
  every `LOAD` segment reports `Align 0x4000` (offsets `0x0`, `0x10f0e0`,
  `0x117b18`). `libmain.so` was checked the same way and reports `0x4000`.
- `unzip -lv build/mocha-testapp-patched.apk`: all seven
  `lib/arm64-v8a/*.so` entries are `Stored`, including `libmain.so` (650,872)
  and `libmocha.so` (4,866,280).
- `zipalign -c -P 16 -v 4` on the patched APK reports
  `Verification successful`; `docs/specifications/apk-patching.md` §4.
- The test emulator uses 16 KiB pages, so the patched APK was installed and
  run in a 16 KiB environment, not only inspected statically.

## Non-claims

- The original Unity libraries' alignment was not modified and is not asserted
  here beyond the test APK's observed 16 KiB `p_align`.
- No claim is made about 64 KiB pages or future page sizes; the build uses
  16 KiB, which is compatible with current 4 KiB and 16 KiB devices.
- Quest hardware was not available at the time of this ADR; 16 KiB behavior was
  exercised only on the emulator. The later Quest 3 run (Android 14, 4 KiB
  pages) loaded and executed the 16 KiB-aligned build, but did not test a
  16 KiB Quest kernel. Quest OS policy beyond page size is verified only for
  that run; see `docs/testing/results-quest-2026-11-20.md`.
