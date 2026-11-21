# Emulator Notes

Date: 2026-11-19 (updated 2026-11-20)
Status: describes the emulator in which the L2/L3 results in
`docs/testing/results-2026-11-18.md` were produced. The later physical-device
run is in `docs/testing/results-quest-2026-11-20.md`.

## 1. Summary

All runtime testing so far was performed on an **x86_64 Android emulator that runs
ARM64 guest code through the Android native bridge (Berberis / `libndk_translation`)**.
It is not an ARM64 kernel and not ARM64 hardware. It is a real Android runtime and a
real Unity IL2CPP player, but the ARM64 instructions are translated.

This distinction is load-bearing for every compatibility claim:

| Claim | Supported by this environment? |
|---|---|
| Android API 26+ native library loading works | Yes |
| Unity 6000.6.9f1 IL2CPP boot sequence is observed and hooked | Yes |
| arm64-v8a code paths execute (under translation) | Yes |
| 16 KiB page-size compatibility of the produced ELF files | Yes (the image uses 16 KiB pages) |
| Quest/VrApi/OpenXR runtime compatibility | **No** — not on the emulator; a later Quest 3 panel-app run is in the Quest results, but VrApi/OpenXR remains untested |
| Behaviour on a physical ARM64 CPU (timing, cache, perf) | **No** — not on the emulator; execution on a physical ARM64 CPU was later observed on Quest 3 |

## 2. Host

| Item | Value |
|---|---|
| Host OS | Arch Linux, x86_64 |
| Virtualisation | KVM (`/dev/kvm` present and writable) |
| ADB | platform-tools from `~/Android/Sdk` |
| AVD name | `Resizable_Experimental` |
| AVD image | x86_64, API level 37 (build 37.2), userdebug |
| AVD memory / cores | 8 GB RAM, 4 cores |
| AVD page size | 16 KiB |
| Emulator | `emulator` from `~/Android/Sdk`, headless boot in roughly 38 s |

The AVD configuration is recorded in `~/.android/avd/Resizable_Experimental.avd/config.ini`.

## 3. Native bridge properties

The properties below were read with `adb shell getprop` on the running AVD. They are
the reason an `arm64-v8a`-only APK installs and launches on an x86_64 image.

| Property | Value |
|---|---|
| `ro.product.cpu.abilist` | `x86_64,arm64-v8a` |
| `ro.dalvik.vm.native.bridge` | `libndk_translation.so` |
| `ro.enable.native.bridge.exec` | `1` |
| Native bridge implementation | Berberis 16.0.0 |

Because `arm64-v8a` appears in `abilist`, the package manager accepts the APK's
`lib/arm64-v8a/` entries. At load time the linker hands ARM64 shared objects to the
native bridge, which maps and translates them.

## 4. Why the test app is arm64-v8a only

Unity 6 removed the Android `x86_64` player architecture. Requesting it silently
produces an ARM64-only APK. The test app is therefore built for `arm64-v8a` only
(`testapp/Assets/Editor/BuildAndroid.cs`) and executed through the bridge. There is
no separate x86_64 build to compare against; the earlier plan to ship one was not
possible with this Unity version.

## 5. What the bridge does and does not give us

It does give us:

- The real Unity `libmain.so` → `libunity.so` → `libil2cpp.so` boot path.
- Real `JNI_OnLoad`, real `il2cpp_init`, real `Il2CppDomain`, 19 assemblies visible.
- Real managed method invocation (`Add`, `Echo`, `Info`) through the IL2CPP C API.
- Real dynamic linking, relocation and `dlopen` behaviour of the Android linker.
- Real 16 KiB page-size enforcement by the image.

It does not give us:

- ARM64 CPU behaviour. Timing-dependent results, cache behaviour and performance
  numbers from the emulator must not be quoted as ARM64 hardware results.
- ARM64 guest frames in native crash dumps. `debuggerd` backtraces of translated
  code do not contain recognisable ARM64 guest frames, so a native crash in the
  loader is diagnosed from `mocha.log` and `logcat` rather than from `tombstone`.
- A Quest runtime. On the emulator there is no VrApi/OpenXR surface, no Quest
  compositor, no Quest OS permission model. The later Quest 3 run launched the
  test player as a panel app, so it still does not exercise an immersive VR
  runtime.

## 6. Root and file ownership

The AVD is a userdebug build, so `adb root` works. The test harness requires it:

```
adb root
adb shell id -u     # must print 0
```

Mod directories are created by the harness and `chown`ed to the app UID
(`cmd package list packages -U dev.mocha.testapp`). The external root
`/sdcard/Android/data/dev.mocha.testapp/files` is FUSE-backed and briefly reports
stale ownership after `chown`; `scripts/run_emulator_test.sh` retries and then
verifies the UID before launching.

## 7. Connecting and reproducing

```
adb devices -l
adb -s emulator-5554 shell getprop ro.product.cpu.abilist
adb -s emulator-5554 shell getprop ro.dalvik.vm.native.bridge
scripts/run_emulator_test.sh --serial emulator-5554
```

`run_emulator_test.sh` refuses to continue unless `arm64-v8a` appears in
`ro.product.cpu.abilist` and the shell is root, so a device that cannot run the
bridge fails loudly instead of producing a misleading result.

## 8. Associated risks

- **Translation differences.** A successful run here does not substitute for a
  physical Quest run. It proves the loader's logic against a real
  Android/IL2CPP runtime. A physical Quest 3 run is recorded separately in
  `docs/testing/results-quest-2026-11-20.md`; it covers one device and one OS,
  so this emulator record remains the 16 KiB evidence.
- **Linker namespace differences.** The observed namespace
  (`clns-9`, permitted paths including `/data`, `/mnt/expand` and the app data
  directory) is the emulator's; OEM devices may differ. The loader's
  internal-staging design (see ADR 0004) exists specifically to stay inside the
  app's permitted paths rather than depending on `/sdcard` being loadable.
- **Page size.** The image enforces 16 KiB. This is representative of newer Android
  devices and stricter than 4 KiB devices, so passing here is the harder case for
  ELF alignment.
