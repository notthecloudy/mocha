# Debugging Mocha

## 1. Where the evidence lives

| Source | Path on device | Notes |
|---|---|---|
| logcat | tag `Mocha` (plus `linker`, `Unity`, `AndroidRuntime`) | every loader log line is mirrored here |
| file log | `<files>/mocha/mocha.log` | timestamped `T/D/I/W/E` lines; appended, rotated at 1 MiB to `mocha.log.1` |
| summary | `<files>/mocha/loader-summary.txt` | one line per discovered mod plus totals; overwritten per launch |
| mod results | `<files>/mocha/<mod>.result` | written by the example mods, not by the loader |
| Unity evidence | `adb logcat -s Unity` | `InitializeScriptEngine OK`, `CodeReloadManager initialized` |

`<files>` is the app's external files directory when writable
(`/storage/emulated/0/Android/data/<pkg>/files`), otherwise internal
(`/data/user/0/<pkg>/files`). The bootstrapping line `files root: …` in the
first `Mocha` messages states which one was selected. The tested runs used
external storage, with mods copied to internal storage for loading (ADR 0004).

## 2. First responses

```
adb shell am force-stop dev.mocha.testapp
adb logcat -c
adb shell am start -W -n dev.mocha.testapp/com.unity3d.player.UnityPlayerGameActivity
adb logcat -v threadtime | grep -E 'Mocha|linker|Unity|AndroidRuntime'
```

Then pull the files:

```
adb pull /sdcard/Android/data/dev.mocha.testapp/files/mocha ./mocha-out
# or, if the bootstrapping line names the internal root:
adb shell run-as dev.mocha.testapp cat files/mocha/mocha.log
```

On a userdebug emulator with `adb root`, both roots are directly readable.

## 3. Reading the startup sequence

A healthy launch has this order (line numbers from the captured run in
`docs/testing/results-2026-11-18.md`):

```
files root: …/files
bootstrapping abi=arm64-v8a api=…
waiting for the runtime gate
libmain shim ready                         (from the shim)
runtime gate installed on libunity.so (dlsym slot, base=…)
IL2CPP runtime gate installed before libunity initializes IL2CPP
libil2cpp.so reported …
il2cpp_init returned the IL2CPP domain (…)
running mod initialization on the il2cpp_init thread      thread T
IL2CPP API resolved
IL2CPP runtime is ready (N assemblies visible)
… per-mod lines …
mod initialization complete discovered=… loaded=… failed=… skipped=…
mod initialization returned to the engine
CodeReloadManager initialized              (Unity)
InitializeScriptEngine OK                  (Unity)
```

Use it as a bisection guide:

| Last line you see | Meaning | Next check |
|---|---|---|
| nothing from `Mocha` | shim did not run or `libmocha.so` was not loaded | was `libmain.so` replaced? correct ABI dir? `adb shell dumpsys package` `primaryCpuAbi` |
| `libmain shim ready` but no `bootstrapping` | `libmocha.so` missing or `mocha_bootstrap` not exported | inspect APK contents and `nm -D` |
| `waiting for the runtime gate` then nothing | gate never installed: `libunity.so` not found/reported, or `dlsym` relocation not patched | grep for `runtime gate installed`; check architecture/ELF of `libunity.so` |
| gate installed, no `il2cpp_init returned` | `libunity` did not call `il2cpp_init`, or the intercept was bypassed | Unity line ordering; whether `libil2cpp.so` was reported |
| `IL2CPP API resolved` but no `runtime is ready` | assembly enumeration did not become available within the timeout | engine variant/version; see `wait_for_runtime` diagnostics |
| mod lines then process dies | a mod crashed or aborted | remove manifests to bisect; see §6 |

If the gate is never installed, nothing else can work: mods are only started
after the gate observes `il2cpp_init` (ADR 0003). Confirm that
`lib/<abi>/libmain.so` in the installed APK is the shim and not the stock
Unity shim.

## 4. Reading `loader-summary.txt`

Format is `key=value`, one per line:

```
mod.<id>=loaded | failed:init | failed:dlopen | failed:entry | skipped:…
```

- `skipped:…` entries record why a manifest was not attempted (missing
  dependency, bad manifest/ABI, duplicate id, cycle).
- `failed:init` means the entry point was called and returned non-zero.
- `failed:dlopen` means the library could not be loaded even after staging.
- `failed:entry` means the entry symbol was missing or the manifest's entry
  could not be resolved.
- `loaded` is written after the entry returned 0.

The summary is only written when the loader reaches the end of the mod loop;
its absence means the process died before that point (or discovery was never
reached).

## 5. Manifest and discovery problems

Discovery is `<files>/mocha/mods/*.json`, sorted lexicographically, each with
a sibling `.so` of the same stem unless the manifest names another `library`
path. A JSON file that fails schema validation is skipped with a reason; it is
not a process failure. Check `docs/specifications/mod-manifest.md` for the
schema and the exact error strings.

If a mod is not mentioned at all:

- is the file extension `.json` (not `.JSON`, not a directory)?
- is the JSON readable by the app UID? On the tested emulator the harness
  chowns the tree; a manually pushed file is owned by `shell` and may be
  unreadable.
- is there a duplicate id (the second is skipped)?
- does the library exist at the expected sibling path?

## 6. Crash triage

A native mod runs in the game process with no sandbox. A crash is a process
death, not a per-mod error. The loader cannot catch `SIGSEGV`/`abort()` from a
mod and continue.

On the tested x86_64 emulator, ARM64 guest frames are translated and
`debuggerd` tombstones do **not** contain useful guest frames
(`docs/testing/emulator-notes.md` §5). Triage from logs instead:

1. The last `Mocha` line names the mod being initialized; entries are run
   synchronously in plan order.
2. Remove manifests one at a time (or move them out of `mods/`) to confirm the
   culprit; an empty directory is a valid launch.
3. For an entry that returns non-zero, the mod's own log/result explains it;
   the loader only records `failed:init`.
4. A crash during `dlopen` (before any entry line) points at the library,
   its dependencies, or the ABI; the linker message names the unresolved
   symbol.

On physical ARM64 hardware, `ndk-stack`/`debuggerd` can symbolize the
tombstone normally; that path was not exercised here.

## 7. Attaching a debugger

The libraries are built `RelWithDebInfo`, so symbols are present. An lldb
session against the app process is possible with the NDK's `lldb-server` and
`adb forward`, but it was not used for the recorded results and is not
documented here as a verified procedure. For the emulator, translating guest
ARM64 code also complicates breakpoints; use log-based triage first.

## 8. Verifying a build before blaming the device

Run the static checks in `docs/development/building.md` §7. The two failures
that look like loader bugs but are packaging bugs:

- `libmain.so` not `STORED`/16 KiB-aligned, or not replaced at all;
- `libmocha.so` absent from `lib/<abi>/` (the shim reports
  `libmocha.so does not export mocha_bootstrap` or a load failure).

## 9. Common messages

| Message | Meaning |
|---|---|
| `libmain shim ready` | shim `JNI_OnLoad` ran; it is the `libmain.so` in the APK |
| `libmocha.so does not export mocha_bootstrap` | wrong/old `libmocha.so` |
| `cannot resolve the data directory` | JNI `getFilesDir` failed; app context/class loader problem |
| `runtime gate installed on libunity.so (dlsym slot, base=…)` | relocation patched successfully |
| `IL2CPP runtime gate installed before libunity initializes IL2CPP` | ordering worked |
| `waiting for the runtime gate` | bootstrap done; waiting for `il2cpp_init` |
| `IL2CPP API resolved` | all required `il2cpp_*` symbols resolved |
| `IL2CPP runtime is ready (N assemblies visible)` | domain and assembly list non-null |
| `il2cpp_init returned the IL2CPP domain (…)` | gate's wrapper ran; mod work follows |
| `mod initialization returned to the engine` | mod loop finished; `il2cpp_init` is returning |
| `loaded from internal copy` | external `dlopen` denied; staging succeeded |
| `copying to internal storage failed: …` | staging failed (permissions/space) |
| `internal copy failed: …` | staged file still not loadable (deps/ABI) |
| `is not accessible for the namespace` | Android linker denial of external storage; expected, triggers staging |

## 10. Debugging the test app itself

`testapp` writes `mocha-test.log` under `Application.persistentDataPath`
(usually `/storage/emulated/0/Android/data/dev.mocha.testapp/files/`) with
`runtime-init`, `awake`, `start` and a heartbeat every 300 frames. If the
loader log is healthy but this file is absent, Unity did not reach the scene,
independent of Mocha. See `docs/development/testapp.md`.
