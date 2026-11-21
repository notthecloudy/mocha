# Mocha Engineering Report

Date: 2026-11-20
Scope: the complete implementation and verification recorded in this
repository. Evidence is in `artifacts/emulator-20261118-191549/`,
`artifacts/quest-20261120-181220/`, and
`artifacts/quest-external-pass-20261120-181426/`, with the commands in
`docs/development/building.md`.

## 1. Executive summary

Mocha is a from-scratch native mod loader for Unity IL2CPP Android/ARM64
players. It is injected by replacing `libmain.so` in the APK, installs a gate
on `libunity.so`'s `dlsym` relocation before Unity asks for `il2cpp_init`,
loads native `.so` mods during `il2cpp_init`, and gives them a versioned C API
onto IL2CPP.

On a physical Meta Quest 3 (Android 14, 4 KiB pages, non-root, user build)
running the same patched APK and a real Unity 6000.6.9f1 IL2CPP ARM64 player,
the loader:

- installed the gate before Unity initialized IL2CPP;
- observed `il2cpp_init`, resolved the IL2CPP API, saw the domain and 19
  assemblies;
- discovered four mods, skipped one for a missing dependency, loaded two,
  failed one by design, and returned to Unity startup;
- hit the Android linker's namespace denial for external `.so` files, staged
  the libraries to internal storage, and loaded them from there;
- invoked managed methods (`Add`, `Echo`, `Increment`, `Info`) and read/wrote a
  static field through the IL2CPP C API;
- reached `InitializeScriptEngine OK` without a crash.

The same results were obtained earlier on a 16 KiB-page x86_64 Android 37
emulator with the ARM64 player under Berberis translation, and again on Quest
using app-internal storage. L1 unit tests pass (87 checks, 0 failures). APK
signature (v2/v3), 16 KiB alignment, ELF `p_align`, and native symbol exports
were verified.

This verifies L4 for one Quest model, one OS release and one minimal test
player. It is not a claim about every Quest device, every title, or immersive
VR runtime behavior; those gaps are stated in §6.4 and `docs/limitations.md`.

## 2. Scope and constraints

In scope: a generic loader, no game-specific assumptions, no H3VR2 code in
core, no multiplayer mod, honest verification levels, reproducible build and
test artifacts.

Out of scope by design: managed C# mods (would require hosting a CLR),
game-specific hooks, anti-cheat/DRM work, and claims beyond the tested Quest
model, OS release and minimal player.

The repository was empty at the start of the project (see
`docs/research/repository-assessment.md`); there was no legacy code to
preserve. Everything under `native/`, `sdk/`, `tools/`, `tests/`, `testapp/`,
`examples/`, and `docs/` was written for this effort.

## 3. Deliverables

| Area | Files |
|---|---|
| Native loader | `native/src/loader/{loader,runtime_gate,il2cpp_api,manifest,plan,semver,paths,log}.cpp` |
| Proxy shim | `native/src/shim/libmain_shim.cpp` |
| Public SDK | `sdk/include/mocha/mocha_api.h` |
| Example mods | `examples/{hello,dependent,failing,missing-dep}-mod/`, `examples/manifests/` |
| Unit tests | `tests/unit/test_main.cpp` |
| APK patcher | `tools/mocha_patch.py` |
| Build scripts | `scripts/build_host.sh`, `scripts/build_android.sh`, `scripts/run_emulator_test.sh` |
| Unity test app | `testapp/` (`MochaTestTarget`, `TestBootstrap`, headless builder) |
| Research | `docs/research/*` (5 documents) |
| Architecture | `docs/architecture/*` (3 documents) |
| Specifications | `docs/specifications/*` (4 documents) |
| Decisions | `docs/decisions/0001`–`0006` |
| Testing | `docs/testing/{strategy,results-2026-11-18,results-quest-2026-11-20,emulator-notes}.md` |
| Development | `docs/development/{building,debugging,testapp}.md` |
| Project docs | `docs/dependencies.md`, `docs/limitations.md`, `docs/roadmap.md`, this report, `README.md` |
| Evidence | `artifacts/emulator-20261118-191549/`, `artifacts/quest-20261120-181220/`, `artifacts/quest-external-pass-20261120-181426/` |

## 4. Architecture

The layers, in boot order:

1. **Proxy `libmain.so`.** Android's `NativeLoader` loads it and calls
   `JNI_OnLoad`. The shim loads `libmocha.so` with `RTLD_NOW | RTLD_GLOBAL`,
   calls `mocha_bootstrap(vm)`, registers the natives Unity's Java layer
   expects, reports `libunity.so` **before** its `JNI_OnLoad`, then reports
   and loads `libil2cpp.so`. (ADR 0001)
2. **Runtime gate.** The loader patches the `dlsym` relocation in
   `libunity.so` so that `il2cpp_init` / `il2cpp_init_utf16` resolve to Mocha
   wrappers. It finds the relocation via `PT_DYNAMIC`/`DT_JMPREL`/`DT_RELA`,
   changes page protection from `/proc/self/maps`, writes the slot, flushes the
   instruction cache, and restores protection. (ADR 0002)
3. **Readiness.** When the real `il2cpp_init` returns a domain, the wrapper
   marks readiness and synchronously calls the registered
   `run_mod_loading` on the same thread, before returning to `libunity`. Mods
   therefore see a live domain on a sanctioned thread, deterministically before
   Unity's script-engine setup. (ADR 0003)
4. **Mod dispatch.** The loader resolves 27 `il2cpp_*` functions (7 required),
   enumerates assemblies, discovers and validates manifests, plans
   dependencies, `dlopen`s each library (staging to internal storage if the
   linker namespace denies the external path), calls the entry point, and
   writes a summary. (ADR 0004, 0005)
5. **Packaging.** The patcher replaces `libmain.so`, adds `libmocha.so`,
   stores every native library uncompressed, aligns to 16 KiB, signs, and
   verifies. (ADR 0006)

## 5. Key decisions

| ADR | Decision | Why |
|---|---|---|
| 0001 | Proxy `libmain.so`, not a `LD_PRELOAD`/root trick | no root, works with stock Android and stock APK load order |
| 0002 | Patch the `dlsym` relocation, not `il2cpp_init`'s code | version-independent, no instruction decoding, installed before the lookup |
| 0003 | Run mods on the `il2cpp_init` thread | one readiness point, legal thread, deterministic ordering; replaces a worker-thread poll |
| 0004 | Stage denied mods in internal storage | Android linker namespaces only permit app-private paths; verified denial on device |
| 0005 | Native `.so` mods behind a C ABI | IL2CPP is AOT; there is no CLR to run `.dll` mods without embedding one |
| 0006 | 16 KiB ELF and ZIP alignment unconditionally | target-class devices use 16 KiB pages; a 4 KiB build fails before Mocha runs |

Each ADR records alternatives and non-claims; they are not marketing choices.

## 6. Verification

### 6.1 Levels

| Level | Environment | Result |
|---|---|---|
| L1 | Host x86_64 | **PASS** — 87 checks, 0 failures |
| L2 | x86_64 API 37, 16 KiB pages, ARM translation | **PASS** — summary and gate evidence |
| L3 | Unity 6000.6.9f1 IL2CPP arm64-v8a on the same emulator | **PASS** — managed calls and results |
| L4 | Meta Quest 3, Android 14, 4 KiB pages, non-root | **PASS** — same APK, gate, assemblies, managed calls |

### 6.2 End-to-end result on the emulator

`artifacts/emulator-20261118-191549/loader-summary.txt`:

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

`hello.result` records `add=2+3=5`, `echo=echo:mocha`, `counter_before=0`,
`counter_after=1`, and an `Info()` string with the Unity version and
persistent path. `dependent.result` records `add=3+4=7`. `failing.result`
records `called=true`, `return=7`. The engine reached
`InitializeScriptEngine OK`.

### 6.3 End-to-end result on Quest 3

The L4 run used `artifacts/quest-external-pass-20261120-181426/` and the same
patched APK whose hashes are recorded in
`docs/testing/results-quest-2026-11-20.md` §3. Device: Quest 3, Android 14
(API 34), `arm64-v8a`, 4 KiB pages, `user`/release-keys, no root.

The loader selected the app's external files directory, installed the gate on
`libunity.so` at `0x7835213000` before IL2CPP initialization, intercepted
`il2cpp_init`, and saw 19 assemblies. The Quest linker namespace (`clns-6`,
permitted paths `/data:/mnt/expand:/data/data/dev.mocha.testapp`) denied direct
`dlopen` of the external mod libraries; each loaded mod was copied to app
internal storage and logged `loaded from internal copy`. The summary is
identical to §6.2: `discovered=4 skipped=1 loaded=2 failed=1`, with the same
`hello.result` and `dependent.result` values. Unity reached
`InitializeScriptEngine OK (0x7945a1bec0)` with no fatal signal or ANR.

An independent pass with mods staged into internal storage
(`artifacts/quest-20261120-181220/`) produced the same summary and engine
result. External files pushed by adb required mode `0666` to be readable by
the app on this user build; that is documented as an operational constraint in
the Quest results, not a loader defect.

### 6.4 Packaging checks

`apksigner verify` (v2/v3 true), `zipalign -c -P 16 -v 4` (success),
`readelf -lW` (`Align 0x4000` on every `LOAD` of both injected libraries), and
all seven native entries `Stored`. Hashes and sizes are in
`docs/testing/results-2026-11-18.md` §3.2; the patched APK and injected
libraries are byte-identical for the emulator and Quest runs.

### 6.5 What the tests do not establish

The physical ARM64 and Quest gaps are closed for the tested configuration, but
not in general: one Quest 3 model on Android 14, one Unity version, one minimal
player, a panel (non-immersive) launch, cold launch only, no unload, no
malicious mods, and no installer UX. Quest 2, Quest Pro, newer Quest OS
releases, 16 KiB-page Quest devices, and commercial titles were not tested.
The unit tests do not cover the gate, the shim, or IL2CPP. See
`docs/limitations.md`.

## 7. Compatibility assessment

| Target | Assessment |
|---|---|
| Android API 26+ | **Partially verified** on API 37 emulator and API 34 Quest; minSdk 26 build, no run on API 26–33 |
| 16 KiB page Android | **Verified** on a 16 KiB emulator image for the injected libraries and APK layout; also loads on the Quest's 4 KiB pages |
| arm64-v8a Unity IL2CPP 6000.6.9f1 | **Verified** under ARM translation and on a physical ARM64 CPU (Quest 3) |
| Other Unity versions | **Unverified**; design is version-independent but the ELF/export assumptions are untested |
| Real ARM64 hardware | **Verified** on Quest 3 (Android 14) |
| Meta Quest | **Verified for the test player** on Quest 3 / Android 14; other models and OS releases unverified |
| Managed/C# mods | **Not supported** by design |
| H3VR2 | **Not integrated**; no game-specific code in core, per scope |

## 8. Security and legal posture

- A mod is unsandboxed native code with the game's privileges; a crash takes
  down the player. This is inherent, not a bug to be "fixed" in-process.
- Mods are executable code from user-writable storage; there is no signature
  or integrity check. Treat the mods directory as trusted input.
- The patched APK is re-signed; original signatures and any integrity/anti-
  tamper checks are invalidated.
- No anti-cheat bypass or DRM circumvention is implemented or intended.
- nlohmann/json (MIT) is the only vendored library; libc++ is statically
  linked under Apache-2.0 with LLVM exception. No LGPL/GPL code is linked.
  A top-level project licence and binary NOTICE are still outstanding.
  Details: `docs/dependencies.md`.

## 9. Repository state

Everything is in the working tree; **nothing was committed, tagged, pushed, or
published** (per instruction). Build outputs and artifact directories are
local. `README.md` gives the entry points; `.gitignore` excludes generated
build and Unity cache directories.

## 10. Remaining work

L4 is complete for Quest 3 / Android 14 with the test player. The highest-value
next steps are a second Unity version, a real title on the same device class,
and the product work below. A physical ARM64 run no longer has to be first,
because Quest 3 provides one. Product work (installer, integrity, lifecycle
hooks) and the managed-runtime question are separate efforts. The ordered list
is in `docs/roadmap.md`.

## 11. Conclusion

The repository contains a working loader verified end to end on both a 16 KiB
emulator and a physical Meta Quest 3: it installs in time, gates the engine,
runs multiple native mods with dependency ordering and failure isolation,
survives Android's linker namespace denial by staging internally, and invokes
real managed code. Its limits are stated rather than papered over. The L4
result is scoped to one Quest model, one OS release and one minimal test
player, not to all Quest titles.
