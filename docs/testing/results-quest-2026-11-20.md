# Test Results — Meta Quest 3, 2026-11-20

This document records the L4 run on physical Quest hardware. It is the
authoritative Quest evidence for this repository. The authoritative end-to-end
artifact directory is:

```
artifacts/quest-external-pass-20261120-181426/
```

A second, independent pass using app-internal storage is in
`artifacts/quest-20261120-181220/`. An earlier external attempt that failed
because of file permissions is retained at
`artifacts/quest-external-20261120-181342/`; it is described in §4.4 and is not
a loader failure.

## 1. Summary

| Level | Scope | Result |
|---|---|---|
| **L1** | Host unit tests | **PASS** — 87 checks, 0 failures |
| **L2** | Android runtime: install, shim, gate, discovery, external load denial, internal staging, summary | **PASS** — see §4 |
| **L3** | Real Unity IL2CPP player: domain, 19 assemblies, lookup, invoke | **PASS** — see §5 |
| **L4** | Meta Quest 3 hardware | **PASS** — see §2–§6 |
| **Static** | Same APK and native binaries as the emulator run | **PASS** — hashes in §3 |

The run installed the same patched APK that was used for the emulator results.
No rebuild was substituted for the device test.

## 2. Environment

From `getprop.txt` and `package.txt` captured with the run:

| Property | Value |
|---|---|
| Device | Meta Quest 3 (`eureka`) |
| Manufacturer / model | Oculus / Quest 3 |
| Android | 14, API 34 |
| Build | `UP1A.231005.007.A1` |
| Fingerprint | `oculus/eureka/eureka:14/UP1A.231005.007.A1/53145330026600520:user/abl_signing_keys:release,amss_signing_keys:release,release-keys` |
| Build type | `user`, release-keys, **not debuggable** (`ro.debuggable=0`) |
| Root | none |
| CPU ABI | `arm64-v8a` |
| Page size | **4 KiB** (`4096`) |
| Package | `dev.mocha.testapp`, versionCode 1 |
| Activity | `com.unity3d.player.UnityPlayerGameActivity` |
| Unity | 6000.6.9f1, IL2CPP |

This is a different environment from the emulator: physical ARM64 rather than
Berberis translation, Android 14 rather than API 37, and 4 KiB pages rather
than 16 KiB. The test APK is debuggable, which permitted `run-as` for evidence
collection and internal staging; the device itself is a stock user build.

## 3. Packaged artifacts

| Artifact | SHA-256 |
|---|---|
| `mocha-testapp-patched.apk` | `599bea7aaaa619af56b0a9288caf84540d2347c64d95938a1461eb33afc0f7fb` |
| `libmocha.so` | `68ff9900e17b89a5175c3c2ad49c4c51b8f8865fce212c4d118b3c9194b71098` |
| `libmain.so` (shim) | `51e276332d465004123f0e3abb974e8f3714c6e904e9292b4841fe6b9ffaba7e` |

These match `artifacts/emulator-20261118-191549/` (see
`docs/testing/results-2026-11-18.md` §3.2). Installation used
`adb install -r build/mocha-testapp-patched.apk` and reported `Success`.

## 4. L2 — runtime evidence on Quest

### 4.1 Timing and threads

All times are from `logcat.txt` on 2026-11-20.

| Line | Event |
|---|---|
| 129 | external root selected: `files root: /storage/emulated/0/Android/data/dev.mocha.testapp/files` |
| 143 | `runtime gate installed on libunity.so` at base `0x7835213000` |
| 144 | gate installed before `libunity.so` initializes IL2CPP |
| 778 | `il2cpp_init returned the IL2CPP domain (0x1)` on thread `20058` |
| 781 | `IL2CPP runtime is ready (19 assemblies visible)` |
| 795 / 843 / 938 | linker denies each external `.so` path (`clns-6`) |
| 829 / 922 / 951 | failing, hello, dependent loaded from internal copies |
| 972 | `mod load complete: discovered=4 loaded=2 failed=1 skipped=1` |
| 988 | Unity `InitializeScriptEngine OK (0x7945a1bec0)` |

`InitializeScriptEngine OK` occurs **after** the loader's summary and return to
the engine, on the main process. The process (`pid 20032` during the run) did
not crash; there is no `FATAL EXCEPTION`, `Fatal signal`, or ANR for the
package in the captured log.

### 4.2 Observed loader summary

`artifacts/quest-external-pass-20261120-181426/loader-summary.txt`:

```
loader_version=0.1.0
abi=arm64-v8a
unity=6000.6.9f1
discovered=4
skipped=1
skipped_mod=mocha.example.missing-dep: missing dependency 'mocha.example.absent'
mod.mocha.example.failing=failed:init
mod.mocha.example.hello=loaded
mod.mocha.example.dependent=loaded
loaded=2
failed=1
```

This is the same result as the emulator run: four discovered, one skipped
before `dlopen`, one intentional init failure, two loaded.

### 4.3 External load denial and internal staging

The Quest linker uses namespace `clns-6`, whose permitted paths are:

```
/data:/mnt/expand:/data/data/dev.mocha.testapp
```

Direct `dlopen` of
`/storage/emulated/0/Android/data/dev.mocha.testapp/files/mocha/mods/*.so` was
therefore denied. The loader copied each discovered library to
`/data/user/0/dev.mocha.testapp/files/mocha/native/<stem>.so`, set mode `0755`,
and loaded it. Every loaded mod logged `loaded from internal copy` (lines
829, 922, 951). This is ADR 0004's staging path operating on Quest hardware,
not merely on the emulator.

The missing-dependency mod was skipped before any `dlopen`; there is no
`missing-dep.result`, as expected.

### 4.4 External file-administration note

External mod files were placed with `adb push` as the `shell` user. The app
could list the directory but could not open the manifests at mode `0660`
(owner `shell`, group `ext_data_rw`); discovery rejected all four with
`cannot be opened`. Setting the files to `0666` made them readable and the run
proceeded. Setting the directory to `0777` was refused by the device
(`Operation not permitted`), which is normal for non-root adb on shared
storage.

This is a deployment detail for a non-root user build, not a loader defect:
the loader correctly reported discovery rejections rather than loading
unreadable manifests. A real installer should create mod files as the app UID
or make them world-readable. The internal-storage pass
(`artifacts/quest-20261120-181220/`) avoids the issue by copying files through
`run-as` into the app's private directory.

## 5. L3 — managed code on Quest

`hello.result` and `dependent.result` were written on Quest:

```
ok=true
add=2+3=5
echo=echo:mocha
counter_before=0
counter_after=1
info=counter=1;unity=6000.6.9f1;persistent=/storage/emulated/0/Android/data/dev.mocha.testapp/files
```

```
ok=true
add=3+4=7
```

`failing.result` contains `called=true` and `return=7`. These are the same
managed calls (`Add`, `Echo`, `Increment`, static `Counter`, `Info`) exercised
in L3 on the emulator, executed here on a physical ARM64 CPU through the
IL2CPP API.

The 16 KiB-aligned libraries loaded and executed on a 4 KiB kernel. This shows
the 16 KiB build is not rejected by a 4 KiB page size; it does **not** prove
16 KiB page behavior on Quest, which this device does not have.

## 6. Validity limits

- **One device and OS.** Quest 3, Android 14/API 34 only. Quest 2, Quest Pro,
  newer Quest OS releases, and future 16 KiB-page Quest builds were not tested.
- **One Unity version and one app.** 6000.6.9f1, minimal single-scene test
  player. No commercial title, store build, split APK, or anti-tamper
  configuration.
- **Panel app, not an immersive VR title.** The test player launched as a
  panel app on Quest. No VrApi/OpenXR-specific initialization, controller
  input, or VR frame-loop interaction was exercised.
- **Cold launch only.** No suspend/resume, scene change, unload, or long-run
  stability. `mocha_shutdown` was not exercised.
- **No adversarial mods.** Mods are trusted native code; no signature or
  integrity check exists.
- **Test-app debuggability.** The APK is debuggable, which enabled `run-as`.
  A release APK cannot be inspected or staged this way; a user-facing installer
  is still outstanding.
- **Permission-sensitive staging.** §4.4 shows adb-pushed external files need
  readable permissions on a user build; this is an operational constraint, not
  a verified end-user path.

## 7. Verdict

On a physical Meta Quest 3 running Android 14 on 4 KiB pages, with no root,
the patched APK installed and started, `libmain.so` loaded the shim, the gate
was installed before Unity initialized IL2CPP, `il2cpp_init` was intercepted,
the IL2CPP API resolved with 19 assemblies, four mods were discovered, one was
skipped for a missing dependency, one failed by design, two loaded and invoked
managed methods, the external linker denial was handled by internal staging,
and Unity reached `InitializeScriptEngine OK` without a crash.

**L4 is verified for this test player on this device and OS.** It is not a
claim about every Quest model, every Unity title, or every Quest OS release.
Those limits are listed in §6 and `docs/limitations.md`.

## 8. Reproduction

No Quest automation script exists; this run was performed manually over `adb`.
The build and patch steps are the same as the emulator procedure in
`docs/testing/strategy.md` §6. The device steps were:

1. `adb install -r build/mocha-testapp-patched.apk` (package
   `dev.mocha.testapp`, launchable activity
   `com.unity3d.player.UnityPlayerGameActivity`).
2. Stage the four mods. For the internal pass, push them to
   `/data/local/tmp/`, then copy them into
   `/data/data/dev.mocha.testapp/files/mocha/mods/` through
   `adb shell run-as dev.mocha.testapp` (the APK is debuggable). For the
   external pass, `adb push` them under
   `/storage/emulated/0/Android/data/dev.mocha.testapp/files/mocha/mods/` and
   make the files world-readable (`chmod 0666`); §4.4 records why.
3. `adb logcat -c`, start the activity with `am start`, and wait for
   `loader-summary.txt` under the app's files root.
4. Pull `logcat.txt`, `mocha.log`, `loader-summary.txt`, each `*.result`, the
   package dump and `getprop.txt` into a timestamped `artifacts/quest-*`
   directory. The two passing directories are named at the top of this
   document.

The procedure depends on the test APK being debuggable for `run-as` and on
readable external files for the external pass. A release APK or a different
installer would change step 2; that path is not implemented.

