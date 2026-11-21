# APK Patching

Status: implemented in `tools/mocha_patch.py`; used to produce
`build/mocha-testapp-patched.apk`; verified with `zipalign -c -P 16` and
`apksigner verify`.

## 1. What patching has to achieve

A stock Unity IL2CPP APK contains, under `lib/<abi>/`:

```
libmain.so          small JNI shim; exports JNI_OnLoad
libunity.so         engine
libil2cpp.so        generated native code for the managed assemblies
libc++_shared.so    C++ runtime
...                 other player libraries
```

Java calls `System.loadLibrary("main")`, which maps `libmain.so` and calls its
`JNI_OnLoad`; the player then calls
`com.unity3d.player.NativeLoader.load(dir)`, implemented in `libmain.so`, which
maps `libunity.so` and `libil2cpp.so`.

Mocha replaces `libmain.so` with its shim and adds `libmocha.so` beside it. No
other entry point is needed and the Android manifest is not modified.

## 2. Why the ZIP layout matters

Android can either extract native libraries at install (`extractNativeLibs=true`)
or map them directly from the APK (`false`). For direct mapping, the linker
requires each library entry to be **uncompressed** and its data offset aligned
to the device page size. Unity builds increasingly ship with direct mapping, and
Android 15+/16 devices may use 16 KiB pages, so the patcher:

1. rewrites **every** `lib/**/*.so` entry as `STORED` (no compression);
2. aligns the archive with `zipalign -P 16 -f 4`;
3. leaves all non-library entries on their original compression.

`-P 16` aligns uncompressed entries to 16 KiB boundaries; the trailing `4`
keeps the older 4-byte alignment of other entries. The resulting `libmocha.so`
and `libmain.so` satisfy both the 4 KiB and 16 KiB cases.

The test APK happens to declare `android:extractNativeLibs="true"` (verified with
`aapt2 dump xmltree`), so install-time extraction is used there. The patched
layout is still valid, and the same patcher covers builds with direct mapping.

Both injected libraries are also built with `-Wl,-z,max-page-size=16384` (see
`native/CMakeLists.txt`), so their own `PT_LOAD` segments are 16 KiB-aligned.
The APK-side alignment and the ELF-side alignment are independent requirements;
both are needed.

## 3. What the patcher does

For `lib/<abi>/` (`--abi`, default `arm64-v8a`):

| Action | Detail |
|---|---|
| Replace `libmain.so` | The shim, stored, mode `0755`, fixed timestamp `1981-01-01T01:01:00`. |
| Add/replace `libmocha.so` | The loader, stored, mode `0755`, same fixed timestamp. |
| Re-store other `.so` | Same bytes, `ZIP_STORED`, original attributes and timestamp. |
| Drop old signatures | `META-INF/*.RSA`, `*.DSA`, `*.EC`, `*.SF`, and `META-INF/MANIFEST.MF`. |
| Keep everything else | Assets, resources, `classes*.dex`, `AndroidManifest.xml`, other ABIs. |

Order of operations:

```
read --apk ──► write unsigned APK (temporary dir)
           ──► zipalign -P 16 -f 4 unsigned aligned
           ──► copy aligned to --out
           ──► apksigner sign --ks … --ks-key-alias …
           ──► apksigner verify --verbose
           ──► assert libmain.so and libmocha.so exist and are STORED
           ──► print size and sha256
```

The input APK is never modified; `--out` must be a different path. All
intermediate files are in a `TemporaryDirectory`.

### Tool discovery

`zipalign` and `apksigner` are resolved in this order: explicit path; 
`~/Android/Sdk/build-tools/36.0.0`; `…/35.0.0`; `PATH`; then the highest
version under `$ANDROID_HOME/build-tools` or `$ANDROID_SDK_ROOT`.

### Signing

Defaults target the standard debug key:

- keystore `~/.android/debug.keystore`
- store and key password `android`
- alias `androiddebugkey`

`--skip-sign` exists for inspection only; an unsigned APK will not install.
Re-signing with a different key changes the app's identity: uninstall the
existing installation first (which deletes app data), or sign with the original
key. `adb install -r` fails with `INSTALL_FAILED_UPDATE_INCOMPATIBLE` otherwise.

Signing behavior is whatever the installed `apksigner` does. On this build
(Build-Tools 36.0.0) the result verified with **v2 and v3** schemes; v1 is
absent. That is sufficient for the API 26+ floor and the API 37 test device.
The v4 `.idsig` sidecar is produced next to the output on some versions; it is
not required for installation.

## 4. Verified result

From the actual artifacts:

- Original `testapp/Build/mocha-testapp.apk`: all six `lib/arm64-v8a/*.so`
  entries deflated.
- Patched `build/mocha-testapp-patched.apk`: all seven entries stored
  (`libmain.so` 650,872 bytes replacing the original 8,720-byte stub;
  `libmocha.so` 4,866,280 bytes added).
- `zipalign -c -P 16 -v 4` reports `Verification successful`; `libmocha.so`
  reports `(OK)`.
- `apksigner verify --verbose` reports v2 `true`, v3 `true`, v1 `false`.
- Manifest unchanged: `minSdkVersion=26`, `targetSdkVersion=36`,
  `extractNativeLibs=true`, `versionName="1.0"`.

## 5. Usage

```fish
python3 tools/mocha_patch.py \
    --apk testapp/Build/mocha-testapp.apk \
    --out build/mocha-testapp-patched.apk \
    --shim build/android/native/libmain.so \
    --mocha build/android/native/libmocha.so
```

Optional flags: `--abi`, `--zipalign`, `--apksigner`, `--keystore`,
`--ks-pass`, `--key-alias`, `--skip-sign`.

Install and launch:

```fish
adb install -r build/mocha-testapp-patched.apk
adb shell am start -n com.DefaultCompany.MochaTestApp/com.unity3d.player.UnityPlayerActivity
```

## 6. Failure modes the script catches

- Input APK, shim, or `libmocha.so` missing.
- `--out` equal to `--apk`.
- `zipalign`, `apksigner`, or their signing steps returning non-zero.
- `apksigner verify` failing.
- Output missing `libmain.so` or `libmocha.so`.
- Either injected library not stored.

It does not catch semantic mistakes: a shim built for the wrong ABI, a
`libmocha.so` with an unresolved `DT_NEEDED` (loading it with `RTLD_NOW` will
fail at runtime and the shim logs it), or a patch applied to a non-Unity APK.

## 7. What patching does not do

- No manifest edits, no new permissions, no `usesCleartextTraffic`, no debuggable
  flag.
- No anti-tamper, DRM, or integrity-check bypass. If a title verifies its own
  libraries, Mocha does not defeat that.
- No multi-ABI handling. Only `--abi` is patched; other ABI directories are
  copied unchanged. A device selecting a different ABI would run the unpatched
  library. The Quest target is `arm64-v8a`, so this is out of scope.
- No recompilation or modification of `libunity.so`, `libil2cpp.so`, or game
  assemblies. The gate changes one relocation slot in memory at runtime, never
  on disk.
- No obfuscation or compression changes outside `lib/`.
- No guarantee that other signers or split APKs behave; only a single-APK debug
  build was exercised.
