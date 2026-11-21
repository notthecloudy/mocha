# Building Mocha

This describes the build that produced the tested artifacts. Commands are
`bash` script invocations; on a `fish` shell run them as shown (the scripts
carry `#!/usr/bin/env bash`).

## 1. Prerequisites

| Component | Requirement | Used for |
|---|---|---|
| Linux x86_64 | Arch Linux in the tested environment | host build, patcher |
| CMake | ≥ 3.22 | all builds |
| Ninja | any recent | all builds |
| C++17 host compiler | g++ or clang | L1 unit tests |
| Android NDK | r27c (`27.2.12479018`) verified; needs lld/clang | loader, shim, example mods |
| Python | 3.10+ | `tools/mocha_patch.py` (stdlib only) |
| Android build-tools | `zipalign`, `apksigner` (36.0.0/35.0.0 searched) | patching/signing |
| adb + emulator or device | with `arm64-v8a` support | L2/L3 |
| Unity | 6000.6.9f1 with Android Build Support + IL2CPP | test app only |

Nothing in the loader build requires Unity. The test app does, and its APK is
already present at `testapp/Build/mocha-testapp.apk`.

`scripts/build_android.sh` resolves the NDK in this order:

1. `$MOCHA_NDK`
2. any `~/Unity/Hub/Editor/*/Editor/Data/PlaybackEngines/AndroidPlayer/NDK`
3. `$ANDROID_NDK_HOME`, then `$ANDROID_NDK_ROOT`

If none contains `build/cmake/android.toolchain.cmake`, it exits with
`error: Android NDK not found; set MOCHA_NDK`.

## 2. Host build and unit tests (L1)

```
scripts/build_host.sh
```

Equivalent to:

```
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

The script runs `ctest`; the binary can also be run directly:

```
./build/host/tests/unit/mocha_unit_tests
```

Expected final line: `87 checks, 0 failures`. Non-zero exit means a failed
assertion. `BUILD_DIR` overrides `build/host`.

The host build compiles only `tests/unit` (root `CMakeLists.txt` selects it
when `ANDROID` is false). It does not need the NDK and does not build the
loader.

## 3. Android build (loader, shim, example mods)

```
scripts/build_android.sh
```

Environment overrides:

| Variable | Default | Meaning |
|---|---|---|
| `MOCHA_NDK` | auto-detected | NDK root |
| `MOCHA_ANDROID_ABI` | `arm64-v8a` | target ABI |
| `MOCHA_ANDROID_API` | `26` | `ANDROID_PLATFORM` |
| `BUILD_DIR` | `build/android` | CMake build directory |

Equivalent to:

```
cmake -S . -B build/android -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/android
```

Outputs:

```
build/android/native/libmain.so      # shim; installed as lib/<abi>/libmain.so
build/android/native/libmocha.so     # loader
build/android/examples/hello-mod.so
build/android/examples/dependent-mod.so
build/android/examples/failing-mod.so
build/android/examples/missing-dep-mod.so
```

The shim is built with `OUTPUT_NAME main` because Android's
`NativeLoader` expects `libmain.so`; the file on disk is
`build/android/native/libmain.so`.

Both injected libraries are compiled with `-Wall -Wextra -Wpedantic -Werror`,
`-fvisibility=hidden`, and linked with:

```
-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384
-Wl,-z,now -Wl,-z,relro
```

Do not remove the page-size flags (ADR 0006). The mods are built by
`examples/CMakeLists.txt` against `sdk/include`.

## 4. Patching the APK

```
python3 tools/mocha_patch.py \
    --apk testapp/Build/mocha-testapp.apk \
    --out build/mocha-testapp-patched.apk \
    --shim build/android/native/libmain.so \
    --mocha build/android/native/libmocha.so
```

Options:

| Option | Default | Notes |
|---|---|---|
| `--abi` | `arm64-v8a` | must match the APK's native dir |
| `--zipalign` | discovered | explicit path if not on `PATH`/SDK |
| `--apksigner` | discovered | explicit path |
| `--keystore` | `~/.android/debug.keystore` | |
| `--ks-pass` | `android` | passed as `pass:…` |
| `--key-alias` | `androiddebugkey` | |
| `--skip-sign` | off | produces an unsigned, aligned APK |

The tool refuses to overwrite the input APK. It replaces
`lib/<abi>/libmain.so`, adds/replaces `lib/<abi>/libmocha.so`, stores every
`lib/**/*.so` uncompressed, runs `zipalign -P 16 -f 4`, signs, verifies the
signature, and checks that both injected libs exist and are `STORED`. It
prints the output size and SHA-256. Details and failure modes:
`docs/specifications/apk-patching.md`.

`apksigner` needs a debug keystore. If `~/.android/debug.keystore` does not
exist, create one with `keytool` (Android Studio normally creates it) and pass
`--keystore`. `--skip-sign` is for inspection only; an unsigned APK will not
install.

## 5. Building the Unity test app (optional)

Only needed if you change `testapp/`. The committed APK was produced with:

```
Unity -batchmode -nographics -projectPath testapp -buildTarget Android \
    -executeMethod MochaTestApp.Editor.BuildAndroid.Build -quit
```

The editor must be 6000.6.9f1 with Android Build Support (SDK/NDK/OpenJDK)
and a valid license. `BuildAndroid.Build`:

- creates the scene if missing (`SceneBootstrap`);
- sets package `dev.mocha.testapp`, minSdk 26, targetSdk auto;
- forces **ARM64 only** and **IL2CPP** (Unity 6 removed Android x86_64);
- uses IL2CPP Release and managed stripping **Low**;
- builds a **Development** APK with `AllowDebugging`;
- writes `testapp/Build/mocha-testapp.apk` and exits non-zero on failure.

The checked-in APK's observed metadata is recorded in
`docs/testing/results-2026-11-18.md` §3.1 and `testapp/Build/` is not
regenerated by the loader build.

## 6. Running the end-to-end test

```
adb root
scripts/run_emulator_test.sh --serial emulator-5554
```

Options: `--serial`, `--wait SECONDS` (default 90), `--no-build` (reuse
`build/android`). The script builds unless `--no-build`, patches, requires
root and `arm64-v8a`, installs, stages the four example mods in both external
and internal app storage, launches the activity, pulls artifacts, and asserts
the summary (`loaded=2`, `failed=1`, no `missing-dep`). It prints `PASS` or
exits non-zero. Environment: see `docs/testing/emulator-notes.md`.

## 7. Verifying the build output

```
readelf -lW build/android/native/libmocha.so | grep LOAD
readelf -lW build/android/native/libmain.so  | grep LOAD
# every LOAD must show Align 0x4000

unzip -lv build/mocha-testapp-patched.apk | grep 'lib/arm64-v8a/.*\.so'
# every entry must be Stored

zipalign -c -P 16 -v 4 build/mocha-testapp-patched.apk
# Verification successful

apksigner verify --verbose build/mocha-testapp-patched.apk
# Verifies; v2 and v3 true
```

## 8. Clean rebuilds

The build directories are ordinary CMake trees; remove `build/host` or
`build/android` and re-run the script to rebuild from scratch. `testapp/` is a
Unity project with a generated `Library/`; deleting `Library/` forces a full
asset reimport and is not necessary for loader work.

## 9. Known build pitfalls

| Symptom | Cause | Fix |
|---|---|---|
| `Android NDK not found` | none of the search paths matched | `export MOCHA_NDK=/path/to/ndk` |
| `zipalign not found` | not on `PATH` and SDK layout unknown | `--zipalign ~/Android/Sdk/build-tools/<v>/zipalign` |
| `apksigner not found` | same | `--apksigner …` |
| `--out must differ from --apk` | output overwrote input | choose a different `--out` |
| link error on page-size flags | old/non-lld linker | use NDK r27c or newer |
| install fails with `INSTALL_FAILED_NO_MATCHING_ABIS` | device lacks arm64-v8a | use an arm64-capable device/emulator |
| app installs but no summary | loader not reached, wrong root, or mods not staged | see `docs/development/debugging.md` |
