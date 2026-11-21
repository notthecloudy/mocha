# Testing Strategy

Status: defines the levels used by `docs/testing/results-2026-11-18.md` and
`docs/testing/results-quest-2026-11-20.md`. It is a description of what was
actually tested, not a wish list.

## 1. Test levels

| Level | Name | Runs on | What it can prove |
|---|---|---|---|
| **L1** | Desktop / unit | Host x86_64, no Android | Parsing, version comparison, dependency resolution, determinism, header offsets |
| **L2** | Android runtime | Android device/emulator | APK install, shim startup, `JNI_OnLoad`, gate installation, `dlopen` order, storage paths, summary output |
| **L3** | Real Unity IL2CPP player | Android + a Unity IL2CPP build | Domain readiness, assembly enumeration, class/method lookup, `runtime_invoke`, thread attachment, Unity version |
| **L4** | Target hardware | Meta Quest | ARM64 execution, Quest OS linker/namespace policy, page-size behavior, real startup on the device |

L2 and L3 are usually observed in the **same run**: the test app is a real
Unity IL2CPP player on a real Android runtime. They are listed separately
because L2 can pass with a non-Unity APK (shim mechanics) while L3 requires the
engine and managed code to work.

L4 **was executed** on a Meta Quest 3 (Android 14, 4 KiB pages, non-root) on
2026-11-20. The same APK as the emulator run was installed, and the device's
own ARM64 CPU executed the loader and the managed calls. This does not cover
Quest 2, Quest Pro, other OS releases, 16 KiB Quest devices, immersive VR
runtime behavior, or commercial titles; see
`docs/testing/results-quest-2026-11-20.md` §6.

## 2. What each level covers

### L1 — host unit tests

`tests/unit/test_main.cpp`, built by `scripts/build_host.sh`, run directly.
It links `semver.cpp`, `manifest.cpp` and `plan.cpp` against the shipped
header and makes **87 assertions** (`CHECK`), reporting
`<n> checks, <n> failures` and exiting non-zero on failure.

Coverage:

- **Versions**: `major.minor[.patch]`, prerelease/build metadata, rejection of
  malformed and `v`-prefixed strings, ordering, `^`, `~`, wildcards, exact,
  ranges, `||`, and error reporting.
- **Manifest schema 1**: required fields, defaults for `entry`/`shutdown`,
  dependency defaults (`*`, optional), rejection of a bad schema, missing
  fields, invalid ids, wrong ABI, and dependency entries without an id.
- **Planning**: dependency-first order, missing dependency, cycle detection,
  version mismatch, optional dependency absent, duplicate ids, invalid
  version, and a three-node chain.

The unit binary does not link Android code, does not touch `dlopen`, and does
not exercise the gate. A green L1 says nothing about runtime behavior.

### L2 — loader mechanics on Android

Exercised end to end by `scripts/run_emulator_test.sh` on a booted device:

1. Install the patched APK, grant/root as needed, push mods and manifests to
   `<external files>/mocha/mods`.
2. Launch the activity, wait for `loader-summary.txt` in the app's external
   files directory.
3. Pull `logcat`, `mocha.log`, the summary and per-mod result files into
   `artifacts/<timestamp>/`.
4. Assert the expected summary content (see §3) and fail otherwise.

L2 evidence that is checked or recorded:

- `JNI_OnLoad` ran and `libmain.so` is the built shim (`libmain shim ready`).
- The runtime gate installed on `libunity.so` before its `JNI_OnLoad`
  (`runtime gate installed …`, `IL2CPP runtime gate installed before libunity
  initializes IL2CPP`).
- `libil2cpp.so` was reported and its `JNI_OnLoad` ran.
- Discovery found four manifests; the summary is written to a stable path.
- External storage was not loadable directly and mods were staged internally
  (linker warning + `loaded from internal copy`).
- A mod whose entry returns non-zero is isolated (`failed=1`) and does not
  prevent the other mods from loading.

### L3 — real Unity IL2CPP

The test app (`testapp/`) is Unity 6000.6.9f1, IL2CPP, ARM64, min SDK 26,
with a `MochaTestTarget` class exposing `Add(int,int)`, `Echo(string)`,
`Increment()` and `Info()`, and a `Counter` static field. The `hello` mod
resolves these through the IL2CPP C API and records the results; the
`dependent` mod depends on `hello` and verifies ordering.

L3 evidence:

- `il2cpp_init returned the IL2CPP domain (0x1)`.
- `IL2CPP runtime is ready (19 assemblies visible)`.
- `Unity version 6000.6.9f1` resolved from the engine assembly.
- `add=2+3=5`, `echo=echo:mocha`, `counter_before=0`, `counter_after=1`, and a
  non-empty `Info()` string, written by the mod.
- Mods ran on the `il2cpp_init` thread and `InitializeScriptEngine OK` follows
  their completion.

### L4 — Quest

Executed on 2026-11-20 on a Meta Quest 3 (`eureka`), Android 14/API 34,
`arm64-v8a`, 4 KiB pages, build `UP1A.231005.007.A1`, `user`/release-keys, no
root. The APK was installed with `adb install -r`; the test APK is debuggable,
which allowed `run-as` for staging and evidence collection.

Two configurations were run:

1. **Internal staging.** Mods were copied through `run-as` into
   `/data/user/0/dev.mocha.testapp/files/mocha/mods`; the loader selected the
   internal root, discovered and loaded from there.
   Artifact: `artifacts/quest-20261120-181220/`.
2. **External discovery with linker-denial fallback.** Mods were placed in
   `/storage/emulated/0/Android/data/dev.mocha.testapp/files/mocha/mods`,
   readable by the app, so the loader selected the external root. The Quest
   linker namespace denied direct `dlopen` of those paths; the loader staged
   each library internally and loaded it. Artifact:
   `artifacts/quest-external-pass-20261120-181426/`.

L4 evidence from the external pass, all in the captured `logcat.txt`:

- gate installed on `libunity.so` at base `0x7835213000` before IL2CPP
  initialization;
- `il2cpp_init returned the IL2CPP domain (0x1)` on the init thread;
- `IL2CPP runtime is ready (19 assemblies visible)`;
- linker denial for each external `.so` with namespace `clns-6` and permitted
  paths `/data:/mnt/expand:/data/data/dev.mocha.testapp`;
- `loaded from internal copy` for each loaded library;
- `mod load complete: discovered=4 loaded=2 failed=1 skipped=1`;
- `InitializeScriptEngine OK (0x7945a1bec0)`;
- no fatal signal or ANR for the package.

See `docs/testing/results-quest-2026-11-20.md` for line numbers, hashes and
limits.

## 3. Pass criteria

The emulator harness prints `PASS` only when all of these hold on the pulled
artifacts:

| Check | Expected |
|---|---|
| `loader-summary.txt` exists | required |
| `loaded=` | exactly `2` |
| `failed=` | exactly `1` |
| `mod.mocha.example.missing-dep=` | absent (it must not be loaded) |
| `missing-dep.result` | absent (its entry must not run) |

In addition, a run is only considered valid if:

- the summary's `discovered=` is `4` and `skipped=` is `1` with the recorded
  reason;
- `hello.result` and `dependent.result` exist with `ok=true`;
- `failing.result` exists with `called=true` and `return=7`;
- `logcat.txt` contains the gate and init lines above, on the expected threads;
- the app reaches Unity's `InitializeScriptEngine OK` (the player was not
  merely alive, it completed startup).

A non-zero exit or a missing/incorrect summary is a failure; the harness does
not treat "the app did not crash" as a pass.

The Quest run used the same summary and result assertions, checked manually
against the pulled artifacts rather than by the emulator harness. It adds
device-specific checks: the gate line must precede IL2CPP initialization in
`logcat.txt`, the linker-denial and `loaded from internal copy` lines must
appear for the external pass, and `InitializeScriptEngine OK` must follow the
loader summary.

## 4. Static and packaging checks

These are not runtime tests but are part of the build gate:

| Check | Tool | Expected |
|---|---|---|
| Segment alignment | `readelf -lW` | every `LOAD` of `libmain.so`/`libmocha.so` has `Align 0x4000` |
| Native entries stored | `unzip -lv` | all `lib/arm64-v8a/*.so` entries `Stored` |
| Archive alignment | `zipalign -c -P 16 -v 4` | `Verification successful` |
| Signature | `apksigner verify --verbose` | v2 and v3 true |
| Exported shim symbols | `nm -D` | `JNI_OnLoad`, `JNI_OnUnload` |
| Exported loader symbols | `nm -D` | `mocha_bootstrap`, `mocha_notify_library_loaded`, `mocha_shutdown`, `mocha_loader_version` |
| Loader dependencies | `readelf -d` | only platform libs (`liblog`, `libdl`, `libm`, `libc`), no `libc++_shared.so` |
| Header layout | L1 header compile | `sizeof(MochaApi)` 72, `MochaIl2CppApi` 224 on LP64 |

## 5. What is deliberately not tested

- **Multiple Unity versions.** Only 6000.6.9f1 was built and run. The gate's
  ELF assumptions and the IL2CPP export set are generic but unverified
  elsewhere.
- **Most ARM64/Quest variation.** Quest 3 on Android 14 is verified, but the
  L2/L3 emulator still translates ARM64 with Berberis, and Quest 2, Quest Pro,
  newer OS releases, 16 KiB Quest hardware, and immersive titles are untested.
  See `docs/testing/results-quest-2026-11-20.md` §6.
- **Malicious mods.** There is no sandbox, no signature verification, and no
  adversarial test corpus. A native mod can crash or compromise the process by
  design.
- **Long-running stability, suspend/resume, lifecycle transitions.** Only a
  cold launch to first scene was exercised.
- **Upgrade/uninstall paths, multiple ABIs, split APKs, other signers.**
- **Shutdown.** The code path exists and is unit-reviewed, but the Android
  activity does not call `JNI_OnUnload` in the captured run; no runtime result
  for `mocha_shutdown` is claimed.
- **Fuzzing.** Manifest/JSON and semver inputs are hand-written cases, not
  fuzzed.

## 6. Reproducing

On a configured host (see `docs/development/building.md`):

```fish
scripts/build_host.sh
./build/host/tests/unit/mocha_unit_tests

scripts/build_android.sh
python3 tools/mocha_patch.py \
    --apk testapp/Build/mocha-testapp.apk \
    --out build/mocha-testapp-patched.apk \
    --shim build/android/native/libmain.so \
    --mocha build/android/native/libmocha.so

scripts/run_emulator_test.sh --serial emulator-5554
```

The emulator harness requires root (`adb root`) and a device whose
`ro.product.cpu.abilist` includes `arm64-v8a`; it refuses to run otherwise.
Artifacts are timestamped under `artifacts/` and include everything needed to
re-check the assertions above without the device.

The Quest pass was manual, because no device-automation script exists yet for a
non-root retail Quest: install the patched APK, stage the four mods (through
`run-as` into internal storage, or as readable files in external app storage),
start the activity, poll for `loader-summary.txt`, then pull `logcat.txt`,
`mocha.log`, the summary and result files. The exact runs and their evidence
are in `docs/testing/results-quest-2026-11-20.md`.

## 7. Evidence retention

Each run directory contains `package.txt`, `logcat.txt`, `mocha.log`, the
loader summary (when written), and each mod's `.result` file. The committed
results document cites the directory and line numbers; it does not paraphrase
logs without the file being present.
