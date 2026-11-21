# ADR 0001: Replace `libmain.so` with a proxy shim

- Status: accepted, implemented
- Date: 2026-11-19
- Code: `native/src/shim/libmain_shim.cpp`, `tools/mocha_patch.py`
- Related: ADR 0002 (runtime gate), `docs/specifications/apk-patching.md`

## Context

A Unity Android player does not execute code from the APK's DEX before native
startup in any hook-friendly way. The startup path is:

1. Java calls `System.loadLibrary("main")`, which maps `lib/<abi>/libmain.so`
   and calls its `JNI_OnLoad`.
2. The stock `libmain.so` registers JNI implementations, including
   `com.unity3d.player.NativeLoader.load(String)Z` and `unload()Z`.
3. The player calls `NativeLoader.load(nativeLibraryDir)`. The implementation
   in `libmain.so` `dlopen`s `<dir>/libunity.so`, calls its `JNI_OnLoad`,
   `dlopen`s `<dir>/libil2cpp.so`, and calls its `JNI_OnLoad`.

Nothing in the APK needs to change for Mocha to run if `libmain.so` itself is
replaced: the Java side calls the same class and method, the manifest has the
same activity, and Unity's own libraries are untouched on disk.

Requirements:

- Loader code must run before `libunity.so`'s `JNI_OnLoad` so the runtime gate
  can be installed before `libunity.so` performs its `dlsym` lookups.
- No modification of `libunity.so`, `libil2cpp.so`, the DEX, or the manifest.
- No root, no `LD_PRELOAD`, no `wrap.sh`, no instrumentation framework.
- Nothing game-specific. The shim may only rely on the public Unity player
  contract (`NativeLoader.load`), not on a title's classes.

## Decision

Build `libmain.so` ourselves as a drop-in replacement that implements the same
`JNI_OnLoad` and the same `NativeLoader` natives, and performs the same library
loads in the same order, with Mocha inserted at the earliest safe point:

```
JNI_OnLoad
  ├─ dlopen("libmocha.so", RTLD_NOW | RTLD_GLOBAL)
  │    (fallback: <dir of libmain.so>/libmocha.so)
  ├─ mocha_bootstrap(vm)
  └─ RegisterNatives(com.unity3d.player.NativeLoader, {load, unload})

NativeLoader.load(dir)
  ├─ dlopen(<dir>/libunity.so, RTLD_LAZY)
  ├─ mocha_notify_library_loaded("libunity.so", handle)   ← gate installed here
  ├─ libunity.so JNI_OnLoad
  ├─ dlopen(<dir>/libil2cpp.so, RTLD_LAZY)
  ├─ mocha_notify_library_loaded("libil2cpp.so", handle)
  └─ libil2cpp.so JNI_OnLoad
```

The notification for `libunity.so` deliberately precedes its `JNI_OnLoad`: the
gate must patch the `dlsym` relocation slot before the engine resolves
`il2cpp_init`. `libmocha.so` is loaded `RTLD_NOW | RTLD_GLOBAL` so its own
dependencies fail early and loudly, and so the IL2CPP symbols it resolves by
`dlsym` come from the global scope.

`JNI_OnLoad` returns `JNI_VERSION_1_6`; a loaded Unity library whose
`JNI_OnLoad` returns a lower version is rejected, matching the stock behavior
of refusing an unsupported JNI version.

The patcher replaces `lib/<abi>/libmain.so` and adds `lib/<abi>/libmocha.so`
(stored, aligned) and re-signs the APK.

## Consequences

Positive:

- One well-defined injection point, present in every Unity Android player.
- Unity binaries are not modified on disk; the only in-memory change is one
  relocation slot (ADR 0002).
- No root, no Java instrumentation, no manifest permission changes.
- The APK remains a normal installable APK.

Negative and risks:

- The shim must track Unity's `NativeLoader` contract. If a future player
  changes the native method signature, the class name, or the library names,
  `RegisterNatives` or the loads fail. The shim logs and returns `JNI_ERR`
  rather than half-working. Only the tested player (Unity 6000.6.9f1, game
  activity) was exercised.
- Some Unity versions may do additional work in the stock `libmain.so`
  (graphics initialization helpers, extra registrations). The shim mirrors what
  the tested player's stub did — resolve `NativeLoader` and load the two
  libraries — and does not attempt to replicate undocumented behavior.
- The shim is ABI-specific; `--abi` must match a directory that exists in the
  APK.
- Re-signing changes app identity; the previous installation must be removed if
  signed with a different key.

## Alternatives considered

- **Patch `libunity.so` on disk** (add a constructor or rewrite an entry):
  version-specific, large, and modifies a proprietary library; also makes the
  patch fail integrity checks. Rejected.
- **Patch the DEX or manifest** to load Mocha from Java: needs `smali`
  rewriting, is more fragile across Unity versions, and can add visible
  changes. Rejected.
- **`LD_PRELOAD` / `wrap.sh`**: requires a debuggable app or a modified
  environment; not available to a normal user install. Rejected.
- **Zygisk / LSPosed / root**: violates the no-root requirement. Rejected.
- **Patch `il2cpp_init` export in `libil2cpp.so`**: still needs a loader
  already running, so it solves ordering only after some other injection
  mechanism exists. Rejected as the primary mechanism.

## Evidence

- `native/src/shim/libmain_shim.cpp:170-213` (`JNI_OnLoad`, registration),
  `:117-155` (`native_load` order and notifications), `:37-73`
  (`libmocha.so` load and symbol resolution).
- Original APK stub `lib/arm64-v8a/libmain.so` is 8,720 bytes; the shim is
  650,872 bytes (`docs/specifications/apk-patching.md` §4).
- The patched APK contains `libmain.so` and `libmocha.so` stored and aligned;
  `apksigner verify` reports v2/v3 true.
- End-to-end launch on the API 37 x86_64 emulator (Berberis ARM translation)
  produced the loader summary and per-mod results; see
  `docs/testing/results-2026-11-18.md`.
