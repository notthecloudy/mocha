# Mocha

A native mod loader for Unity IL2CPP Android players (ARM64). Mocha is
injected by replacing `libmain.so` in the APK, intercepts `il2cpp_init` before
Unity uses it, and loads native `.so` mods with manifest-based discovery,
dependency ordering, and per-mod failure isolation.

**Status: experimental, version 0.1.0.**
L1 passes on the host; L2/L3 are verified on an x86_64 Android 37 emulator with
a real Unity 6000.6.9f1 IL2CPP ARM64 player running through ARM translation.
**L4 passes on a physical Meta Quest 3** (Android 14, 4 KiB pages, non-root)
running the same patched APK. That result covers one Quest model, one OS
release and one minimal test player; broader titles and devices remain
untested. See [Verification](#verification) and
[docs/limitations.md](docs/limitations.md).

## How it works

1. `libmain.so` is replaced by a shim that loads `libmocha.so` and reports
   `libunity.so` before its `JNI_OnLoad`.
2. Mocha patches `libunity.so`'s `dlsym` relocation so `il2cpp_init` resolves
   to a wrapper (ADR 0002).
3. When the real `il2cpp_init` returns a domain, mods run synchronously on
   that thread: discovery, manifest validation, dependency planning, `dlopen`,
   entry calls.
4. Mods receive a versioned C API table onto IL2CPP (`sdk/include/mocha/`).

Mods are native shared libraries, not managed `.dll` files: an IL2CPP player
ships no CLR (ADR 0005).

## Requirements

- Android 8.0 (API 26) or later, `arm64-v8a`.
- For building: Android NDK r27c (27.2.12479018), CMake 3.22+, Ninja, a C++17
  compiler for host tests, Python 3.10+, Android `zipalign`/`apksigner`.
- Optional: Unity 6000.6.9f1 with the Android module to rebuild the test app.

## Quick start

```sh
scripts/build_host.sh          # unit tests (87 checks)
scripts/build_android.sh       # libmain.so, libmocha.so, example mods

python3 tools/mocha_patch.py \
    --apk testapp/Build/mocha-testapp.apk \
    --out build/mocha-testapp-patched.apk \
    --shim build/android/native/libmain.so \
    --mocha build/android/native/libmocha.so

scripts/run_emulator_test.sh --serial emulator-5554
```

The patcher stores native libraries uncompressed, aligns the APK to 16 KiB,
signs it with a debug key, and verifies the result. The end-to-end script
installs the APK, stages the example mods, launches the app, and checks the
loader summary. Full instructions, including environment overrides and
troubleshooting, are in [docs/development/building.md](docs/development/building.md).

Mods live in `<external app files>/mocha/mods/` as `<id>.json` plus a sibling
`<name>.so`. On linker-namespace denial the library is copied to app-internal
storage and loaded from there (ADR 0004).

## Verification

| Level | What | Result |
|---|---|---|
| L1 | Host unit tests (semver, manifests, planning) | **PASS** — 87 checks, 0 failures |
| L2 | Android loader behavior on emulator | **PASS** |
| L3 | Real Unity 6000.6.9f1 IL2CPP ARM64 player | **PASS** |
| L4 | Meta Quest 3 hardware (Android 14, 4 KiB pages) | **PASS** |

The emulator run observed the gate installed before IL2CPP initialization,
`discovered=4 skipped=1 loaded=2 failed=1`, successful managed calls, and
`InitializeScriptEngine OK`. The Quest run used the same APK and observed the
same summary, the gate installed before `il2cpp_init`, 19 assemblies, the
external linker denial handled by internal staging, and
`InitializeScriptEngine OK`; it has a 4 KiB page size and a physical ARM64 CPU.
The emulator is x86_64 API 37 with 16 KiB pages, where the arm64-v8a player ran
via Berberis/libndk_translation. Evidence:
[docs/testing/results-2026-11-18.md](docs/testing/results-2026-11-18.md) and
[docs/testing/results-quest-2026-11-20.md](docs/testing/results-quest-2026-11-20.md).
Strategy and non-tests: [docs/testing/strategy.md](docs/testing/strategy.md).

## Repository layout

| Path | Contents |
|---|---|
| `native/` | loader (`libmocha.so`) and proxy shim (`libmain.so`) |
| `sdk/include/mocha/` | public mod C ABI |
| `examples/` | hello, dependent, failing, missing-dependency mods and manifests |
| `tests/` | host unit tests |
| `tools/mocha_patch.py` | APK patcher, aligner, signer, verifier |
| `scripts/` | host build, Android build, emulator end-to-end |
| `testapp/` | minimal Unity IL2CPP ARM64 test project |
| `artifacts/` | captured test runs and logs |
| `docs/` | research, architecture, specs, ADRs, testing, development |

## Documentation

- [Engineering report](docs/engineering-report.md)
- [Architecture overview](docs/architecture/overview.md) and
  [boot sequence](docs/architecture/boot-sequence.md)
- [Mod ABI](docs/specifications/mod-abi.md),
  [manifest schema](docs/specifications/mod-manifest.md),
  [dependency resolution](docs/specifications/dependency-resolution.md),
  [APK patching](docs/specifications/apk-patching.md)
- Decisions: [0001](docs/decisions/0001-proxy-libmain-shim.md),
  [0002](docs/decisions/0002-dlsym-relocation-gate.md),
  [0003](docs/decisions/0003-runtime-ready-on-init-thread.md),
  [0004](docs/decisions/0004-internal-staging.md),
  [0005](docs/decisions/0005-native-abi-over-managed.md),
  [0006](docs/decisions/0006-16k-page-alignment.md)
- [Building](docs/development/building.md),
  [debugging](docs/development/debugging.md),
  [test app](docs/development/testapp.md)
- [Limitations](docs/limitations.md), [roadmap](docs/roadmap.md),
  [dependencies and licences](docs/dependencies.md)

## Scope and non-goals

- Generic loader: no game identifiers, game-specific assemblies, hooks, or APIs
  in core. H3VR2 is not integrated; any such work is separate and out of scope.
- No multiplayer mod was designed, implemented, or researched.
- No managed/C# mod support (would require hosting a CLR).
- No anti-cheat bypass, DRM circumvention, or detection evasion.
- Patched APKs are re-signed; store signatures, Play Integrity, and anti-tamper
  checks will be invalidated or refuse the build.

Mods are unsandboxed native code running with the game's privileges. Treat the
mods directory as trusted input. See [docs/limitations.md](docs/limitations.md).

## Licensing

No top-level `LICENSE` file exists yet. nlohmann/json (MIT) and the statically
linked libc++ (Apache-2.0 with LLVM exception) carry notice obligations; the
Unity test APK is not redistributable without checking Unity's terms. Details:
[docs/dependencies.md](docs/dependencies.md).

## Project status

All work is in the working tree. Nothing has been committed, tagged, pushed, or
published.
