# Hooking and Injection: Prior Art and Design Space

Research date: 2026-11-19
Scope: the techniques that exist for getting native code into a Unity Android
process and running it at a controlled point. This document records why Mocha
chose APK-level library substitution plus a `dlsym` relocation gate, and why it
rejected the alternatives. It is a design-space survey, not a claim that every
technique listed was implemented.

## 1. The two separate problems

"Modding a game" hides two independent problems:

1. **Injection** — get your code mapped into the game process at all.
2. **Interception** — run your code at a specific moment relative to the
   engine's own initialization.

They have different constraints. A technique can solve injection and be useless
for interception, or vice versa. Mocha solves both without root and without a
custom ROM, which rules out a large part of the design space up front.

## 2. Injection options

| Technique | Requirement | Applies to a stock APK? | Notes |
|---|---|---|---|
| Substitute `libmain.so` inside a repackaged APK | Ability to rebuild and re-sign the APK | Yes | Chosen. Works on non-rooted devices. The app's own `System.loadLibrary("main")` loads the replacement. |
| Add an extra library + a loader that dlopens it | Something must call `dlopen` on it | Only with another hook | Not sufficient alone. |
| `LD_PRELOAD` | Control of the process environment | No | Android apps are launched by Zygote with a fixed environment; `LD_PRELOAD` is not honoured for app processes. |
| Zygisk / Magisk module | Root, Magisk, per-app injection enabled | No (on a clean device) | Powerful but depends on a rooted device and a third-party framework. Outside Mocha's target. |
| `ptrace` attach | Root or a debuggable app | Partially | `run-as`/debug builds only in practice; fragile on modern Android; anti-tamper hostile. |
| Frida / dynamic instrumentation | Root or a repackaged app with the agent | Partially | A tool for analysis, not a shipping loader. |
| Replace or wrap a library the app already loads | Repackage the APK | Yes | Generalises Mocha's chosen approach. `libmain.so`, `libunity.so` and `libil2cpp.so` are all candidates; `libmain.so` is the smallest and most stable surface. |

Mocha picks the last row with `libmain.so` as the target because:

- It is tiny and its ABI is narrow (`JNI_OnLoad`, plus two JNI methods).
- It is loaded first, before either large library, which gives the loader the
  earliest possible interception window.
- Its behaviour is a documented contract visible in the APK itself: load
  `libunity.so`, load `libil2cpp.so`, invoke each `JNI_OnLoad`. A replacement
  only has to reproduce that.

The cost is that the APK must be patched and re-signed, so the device must
accept the new signature. That is a distribution problem (users install a
patched build), not a runtime-exploit problem.

## 3. Interception options once code is mapped

| Technique | Granularity | Survives symbol changes? | Risk |
|---|---|---|---|
| Export the same symbol from the substituted library (proxy export) | Whole function | Yes, if the contract is stable | Only works for symbols the loader itself resolves through the substituted library. |
| Rewrite a PLT/GOT relocation slot | Per imported symbol | Yes, matches by symbol name | Requires the target to import the symbol dynamically; needs page-permission handling. |
| Inline detour (trampoline) | Per function body | No, depends on instruction bytes | Needs a hooking engine (Dobby, And64InlineHook); breaks with compiler/instrumentation changes; harder on AOT code. |
| `dlsym` interposition via dynamic linker API | Every `dlsym` call | Yes | Global and blunt; affects the whole process. |
| Debugger / breakpoint | Per address | No | Needs a debugger, not shippable. |

Mocha uses **proxy export plus one relocation slot**:

- Proxy export: the shim itself is `libmain.so`, so `JNI_OnLoad` and the
  `NativeLoader` natives are Mocha's code by construction. No hooking needed.
- Relocation slot: the shim loads `libmocha.so`, then before loading
  `libunity.so` it notes that library's `dlsym` import. When `libunity.so` later
  calls `dlsym(handle, "il2cpp_init")`, the call lands in Mocha's wrapper, which
  returns Mocha's `il2cpp_init` wrapper. The wrapper calls the real function and
  then runs mod initialization.

The second step is a one-slot patch, not a hook of `dlsym` itself. `libunity.so`
keeps its own `dlsym`; only the address it resolved for that import is replaced.
This is why the gate logs a base address and a slot patch rather than an
instruction patch.

ADR 0002 records the decision and its rejected alternatives.

## 4. Prior art (verified 2026-11-19)

The loaders below were read from their own repositories on 2026-11-19. License
identifiers are the ones their repositories state.

| Project | Licence | Platform / runtime | What it does | Relevance to Mocha |
|---|---|---|---|---|
| **UnityDoorstop 4** | LGPL-2.1 | Windows, Linux, macOS; Mono and IL2CPP | Injects a managed assembly before Unity starts; for IL2CPP it hosts CoreCLR and bridges to the IL2CPP runtime. Its README states that IL2CPP cannot run managed assemblies, which is why a CLR is hosted. | Confirms the AOT limitation and the "host a second runtime" strategy that Mocha deliberately did not take. |
| **BepInEx** | LGPL-2.1 | Windows/Linux for IL2CPP; macOS and ARM listed unsupported | Plugin framework built on Doorstop 4.x (4.6.0 at time of reading), HarmonyX 2.10.2, MonoMod; uses Cpp2IL (v2022.1.0) and Il2CppInterop (v1.5.3) for IL2CPP. | The reference implementation of managed modding on IL2CPP. Shows the size of the subsystem Mocha avoided. Its own platform matrix excludes ARM, which is exactly Mocha's target. |
| **Il2CppInterop** | LGPLv3 | Windows/Linux, .NET | Generates .NET assemblies from Cpp2IL output and bridges calls between CoreCLR and IL2CPP. | The interop layer any managed-on-IL2CPP loader needs. Out of scope for Mocha's native ABI. |
| **MelonLoader** | Apache-2.0 | Windows (proxy DLLs), macOS (DYLD `dlsym` interpose); Android/Quest described as work in progress | Universal Mono/IL2CPP loader. Bundles Dobby (Apache-2.0) and plthook (BSD-2) for native hooking, Cpp2IL (MIT) and Il2CppInterop (LGPLv3). | Closest prior art in technique: on macOS it interposes `dlsym`, which is the same class of mechanism as Mocha's relocation gate. Its Windows path (proxy DLL) is the same class as Mocha's proxy `libmain`. Its Android support is not established, so it is not evidence that the Quest path works. |

Two conclusions follow:

- The *techniques* Mocha uses are not novel; proxy libraries and `dlsym`
  interposition are established practice across these projects.
- The *combination* Mocha needs — non-rooted Android, ARM64, no managed runtime
  — is not covered by any of them. BepInEx excludes ARM; MelonLoader's Android
  support is WIP; UnityDoorstop targets desktop. Mocha's contribution is a small,
  documented implementation of the native-ABI path for this combination.

## 5. Why not inline hooking

Dobby and plthook are the standard native hooking libraries in this space, and
MelonLoader ships both. Mocha does not use either:

- Inline detours depend on the exact instruction sequence at the target. On
  AOT-generated ARM64 code that sequence is a build artifact of the Unity
  version and the IL2CPP compiler, so a hook that works for one player can
  corrupt another. The blast radius of a bad inline patch is arbitrary code
  execution in the player process.
- The interception Mocha needs is at a single well-defined boundary: the
  `il2cpp_init` lookup. A relocation-slot patch reaches that boundary exactly
  and does nothing else. There is no need for a general hooking engine.
- A smaller dependency surface means a smaller licence and audit surface.
  Mocha's loader has no third-party native dependencies at all (see
  `docs/dependencies.md`).

If future work needs to hook functions that are not reached through a dynamic
lookup, an inline engine becomes necessary. That is recorded in the roadmap, not
implemented.

## 6. Why not `dlsym` interposition globally

MelonLoader's macOS path interposes the process's `dlsym`. Mocha does not,
because on Android the shim runs before most of the process has resolved
anything, and a global `dlsym` hook would affect every library in the app,
including the Unity player and any anti-tamper code. A single relocation slot
achieves the same interception for the one caller that matters, with a much
smaller blast radius.

## 7. Evidence

The mechanism was exercised on the test app. Log excerpts
(`artifacts/emulator-20261118-191549/mocha.log`):

```
libmain shim ready
loading Unity libraries from /data/app/.../lib/arm64
runtime gate installed on libunity.so (dlsym slot, base=0x...)
libil2cpp.so reported
intercepted libunity il2cpp_init lookup
intercepted libunity il2cpp_init_utf16 lookup
```

The gate installs before `libil2cpp.so` is loaded, which is what makes the
interception effective: by the time the player resolves `il2cpp_init`,
`libunity.so`'s slot already points at Mocha.

## 8. What is not claimed

- No claim that Mocha works on a non-rooted retail device in general. The test
  AVD was a userdebug image, and the later Quest 3 run used a non-root user
  build but a debuggable test APK. APK re-signing and installation on a locked
  retail title is a distribution concern that was not exercised.
- No claim about anti-tamper or integrity-checking games. A repackaged APK
  changes the signature and the library bytes; any title that verifies either
  will reject it.
- No claim about 32-bit ABIs. Only `arm64-v8a` was built and run.
- No claim about MelonLoader's Android/Quest status beyond what its repository
  said on 2026-11-19.
- Quest was unverified at the research date. Update (2026-11-20): the
  relocation gate and the injection path were exercised on a physical Meta
  Quest 3 (Android 14, 4 KiB pages, non-root) running the test player; see
  `docs/testing/results-quest-2026-11-20.md`. Immersive VR titles, other Quest
  models and other OS releases remain untested; see `docs/limitations.md`.
