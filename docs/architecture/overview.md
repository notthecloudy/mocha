# Architecture Overview

Mocha is a native mod loader for Unity IL2CPP Android players, built for
`arm64-v8a` in the Meta Quest target class. It has been verified end to end on
a physical Meta Quest 3 (Android 14, 4 KiB pages, non-root), as well as on a
16 KiB-page emulator; the device result is recorded in
`docs/testing/results-quest-2026-11-20.md`, and the scope of that claim is in
`docs/limitations.md`. This document describes the
components, the boundaries between them, and the constraints that shaped the
design. The step-by-step runtime sequence is in `boot-sequence.md`; the state
machine is in `loader-lifecycle.md`.

## 1. Goals and non-goals

Goals:

- Get a stable native entry point inside a stock Unity IL2CPP player without
  root.
- Discover, validate, order and initialize native mods from a filesystem
  directory.
- Give mods a versioned C ABI over the IL2CPP runtime.
- Isolate recoverable mod failures (failed `dlopen`, missing symbol, non-zero
  init): one bad mod must not prevent the other mods from loading. A native
  fault inside a mod cannot be contained in-process.
- Keep the core free of game-specific knowledge.

Non-goals (implemented or researched elsewhere, deliberately absent here):

- Loading managed C# assemblies. IL2CPP is AOT-only and ships no CLR; hosting
  one is a separate subsystem (see ADR 0005 and `docs/research/unity-il2cpp-loading.md`).
- Multiplayer, networking, or game-specific integrations.
- Any H3VR2 identifier, hook, assembly name or API in the core. The loader names
  no game. Everything game-specific is data (a manifest) or a mod's own code.
- Bypassing anti-tamper, DRM, or platform integrity checks.
- 32-bit ABIs, Mono players, or non-Android platforms.

## 2. Layers

```
┌──────────────────────────────────────────────────────────────┐
│ Mods (third-party .so files, C ABI)                          │
├──────────────────────────────────────────────────────────────┤
│ SDK: sdk/include/mocha/mocha_api.h                           │
│      MochaApi, MochaIl2CppApi, log, init/shutdown signatures │
├──────────────────────────────────────────────────────────────┤
│ Loader: native/src/loader                                    │
│   bootstrap · discovery · manifest · plan · semver · log     │
│   runtime gate · mod lifecycle · summary                     │
├──────────────────────────────────────────────────────────────┤
│ Shim: native/src/shim/libmain_shim.cpp                       │
│   replaces lib/<abi>/libmain.so                              │
├──────────────────────────────────────────────────────────────┤
│ Unity player: libunity.so · libil2cpp.so                     │
└──────────────────────────────────────────────────────────────┘
```

Data flows downward: a mod calls the SDK, which dispatches into IL2CPP through
resolved function pointers. Control flows upward: Unity calls `JNI_OnLoad`,
`NativeLoader.load`, `il2cpp_init`; each is a point where Mocha inserts itself.

## 3. Components

| Component | Path | Responsibility |
|---|---|---|
| `libmain` shim | `native/src/shim/libmain_shim.cpp` | Implements `JNI_OnLoad`, `NativeLoader.load`/`unload`; loads `libmocha.so`; loads Unity's libraries and notifies the loader before each `JNI_OnLoad`. |
| Bootstrap | `native/src/loader/loader.cpp` (`mocha_bootstrap`) | Resolves data/internal roots, creates directories, initializes logging, registers the runtime-ready callback. |
| Runtime gate | `native/src/loader/runtime_gate.cpp` | Finds `libunity.so`, rewrites its `dlsym` relocation slot, intercepts `il2cpp_init`/`il2cpp_init_utf16`, signals completion. |
| Mod loader | `native/src/loader/loader.cpp` (`run_mod_loading`) | Waits for the domain, resolves the API, discovers and orders mods, loads and invokes each, writes the summary. |
| Discovery | `loader.cpp` (`discover_mods`) | Enumerates `*.json` in the mods directory, sorts deterministically, pairs each with its sibling `.so`. |
| Manifest | `native/src/loader/manifest.{h,cpp}` | Parses and validates schema 1. |
| Dependency plan | `native/src/loader/plan.{h,cpp}` | Disables invalid/duplicate/missing-dependency mods; produces a deterministic topological order; reports cycles. |
| Version logic | `native/src/loader/semver.{h,cpp}` | Semver parsing and constraint satisfaction. |
| Logging | `native/src/loader/log.{h,cpp}` | Leveled log to logcat and a rotating file. |
| SDK | `sdk/include/mocha/mocha_api.h` | The only header a mod includes. |
| Patcher | `tools/mocha_patch.py` | Replaces `libmain.so`, adds `libmocha.so`, realigns, signs. |
| Test app | `testapp/` | Unity 6000.6.9f1 IL2CPP Android project with known managed methods. |
| Example mods | `examples/` | Four mods exercising success, failure, dependency and missing-dependency paths. |

The loader library has no dependency on the shim and vice versa; they meet at
three exported functions resolved with `dlsym`:

```c
void mocha_bootstrap(JavaVM* vm);
void mocha_notify_library_loaded(const char* name, void* handle);
void mocha_shutdown(void);
```

## 4. Design principles

**Native-first ABI.** Mods compile against a C header and get function pointers,
not C++ objects. There is no shared C++ runtime between loader and mod, no STL
across the boundary, and no exceptions escaping a mod entry point. This keeps
mods buildable with different toolchains and avoids ABI drift.

**Append-only versioning.** `MochaIl2CppApi` and `MochaApi` carry `struct_size`.
New functions go at the end. A loader fills what it knows; a mod reads up to its
own size. `MOCHA_ABI_VERSION` guards incompatible changes.

**Deterministic ordering.** Discovery sorts manifest paths; dependency
resolution picks the lexicographically smallest ready id at each step. Two runs
with the same directory produce the same order, which makes failures
reproducible and the summary diffable.

**Failure isolation.** Every mod is loaded and invoked inside a try/catch and a
result check. A missing symbol, a failed `dlopen`, a bad entry point or a
shutdown mismatch marks that mod failed (or skipped) and the loader continues.
The artifacts show this working: `failed.mod` returns 7 from init, and the two
healthy mods still load.

**Fail closed at the gate.** If the gate cannot be installed, or the runtime does
not initialize within the timeout, mods are not run and the reason is logged.
Mocha does not guess at readiness.

**No game knowledge.** The loader knows assembly names only as data supplied by
mods. The single engine-level name it uses is `UnityEngine.CoreModule.dll`, to
read `Application.unityVersion`; nothing game-specific is compiled in.

## 5. Trust and threat model

Mocha assumes the APK is patched and re-signed by the user, and that mods are
code the user chose to install. Native mods run with the app's full privileges;
there is no sandbox between mods. The validation in the manifest layer is about
*correctness and diagnosability*, not security: it prevents a typo from
producing undefined load order, not a malicious mod from doing damage. This is
stated plainly in `docs/limitations.md`.

The loader does not download anything, does not contact a network, and does not
modify files outside its own data directory and the mods directory it reads.

## 6. Repository layout

```
native/src/shim/       libmain replacement
native/src/loader/     loader, gate, manifest, plan, semver, log
sdk/include/mocha/     public mod ABI
examples/              example mods and shared helper header
testapp/               Unity IL2CPP test project
tests/unit/            host unit tests (semver, manifest, plan)
tools/                 APK patcher
scripts/               host/Android build and emulator test scripts
docs/                  research, architecture, specs, ADRs, testing
artifacts/             captured test output
```

## 7. Where to read next

- `boot-sequence.md` — the exact call order from process start to mod entry.
- `loader-lifecycle.md` — states, transitions, threading and shutdown.
- `docs/specifications/mod-manifest.md` — the manifest contract.
- `docs/decisions/` — why the shim, the gate, the thread and the staging
  directory are what they are.
