# Roadmap

Status as of 2026-11-20. "Done" means implemented and evidenced; "next" is the
order that follows from the current limitations, not a commitment. L4 (Quest)
is done for Quest 3 / Android 14 with the test player; broader device and
title coverage remains open.

## Done

| Milestone | Evidence |
|---|---|
| Research from primary sources (Unity IL2CPP, Android loading, injection, prior art) | `docs/research/*` with dates and sources |
| Architecture and boot sequence | `docs/architecture/*` |
| Proxy `libmain.so` shim | `docs/decisions/0001`, run log `libmain shim ready` |
| Gate on `libunity`'s `dlsym` relocation before `JNI_OnLoad` | `docs/decisions/0002`, `runtime gate installed …` |
| Mod initialization on the `il2cpp_init` thread | `docs/decisions/0003`, same-thread log evidence |
| Internal staging for linker-namespace denial | `docs/decisions/0004`, linker warnings + `loaded from internal copy` |
| Native mod ABI v1 and example mods | `docs/specifications/mod-abi.md`, three mods loaded |
| Manifest schema 1, semver, deterministic dependency planning | `docs/specifications/*`, 87 unit checks |
| Failure isolation for init failure and missing dependency | `loaded=2 failed=1 skipped=1` run |
| 16 KiB ELF and ZIP alignment | ADR 0006, `readelf`/`zipalign` checks |
| APK patcher with signing and verification | `tools/mocha_patch.py`, v2/v3 signature |
| Unity 6000.6.9f1 ARM64 IL2CPP test app | `testapp/`, L3 results |
| **L4 on physical Meta Quest 3** (Android 14, 4 KiB, non-root) | `docs/testing/results-quest-2026-11-20.md`, two artifact directories |
| Test levels, strategy, results, emulator notes | `docs/testing/*` |
| Dependency/licence inventory | `docs/dependencies.md` |

## Next: close the verification gap

1. **A second Unity version** (an LTS adjacent to 6000.6.9f1). Rebuild the
   test app, re-run, and confirm the gate's relocation handling and required
   `il2cpp_*` exports. Record any export-name differences.
2. **One real IL2CPP title** in a lawful test configuration, to catch
   differences a minimal app cannot (multiple scenes, larger assembly set,
   stripped methods, immersive VR initialization).
3. **Other ARM64 targets and Quest models.** A phone/tablet and a Quest 2 or
   Quest Pro would test linker, page-size, and OS variation beyond the one
   Quest 3 / Android 14 configuration.

## Next: product hardening

4. **Installer/patcher UX.** A small CLI/GUI that selects an APK, checks ABI
   and page size, patches and signs, and reports what changed. The Python
   patcher is the engine; this is packaging.
5. **Mod installation path.** A documented, user-facing way to place mods
   without root/adb (SAF picker or in-app import), preserving app-UID ownership
   and the external/internal distinction.
6. **Integrity and provenance.** At minimum a manifest checksum and an
   optional signature check before staging; otherwise keep the current
   explicit "trusted input" warning. This is a security design decision, not a
   quick patch.
7. **Config and diagnostics.** Log level/path configuration, a machine-readable
   diagnostics dump (ABI, page size, paths, gate result, summary), and a
   support bundle.
8. **Staging lifecycle.** Cleanup of stale internal copies, collision
   detection at discovery time, and a defined update story.
9. **Lifecycle hooks.** A supported later point than `il2cpp_init` (scene
   load / player loop) without game-specific hooks. This needs an engine-
   version-tolerant mechanism and its own ADR; it is the most likely source
   of per-version breakage.

## Next: ABI and ecosystem

10. **ABI v2, additive.** More of the 243 `il2cpp_*` functions
    (type/class/field helpers, string and array helpers, exception access),
    with the append-only struct rule and a new `MOCHA_ABI_VERSION`.
11. **SDK ergonomics.** A small C++ wrapper over the table, a mod template,
    and a build example for CMake. No managed layer unless item 14 happens.
12. **Overload and generic lookup.** A documented resolution helper (by
    parameter types, not only arity) to reduce the name+arity limitation.
13. **Crash containment for mod initialization.** A process cannot survive a
    native fault in-process; the realistic options are a mod-init subprocess
    (not possible for engine calls) or accepting the limitation. Keep the
    current behavior and document it.

## Exploratory (not scheduled)

14. **Managed mods via a hosted runtime (CoreCLR/Mono) and generated IL2CPP
    interop.** This is the UnityDoorstop/BepInEx model and a project in itself:
    tens of MB, two GCs, per-version interop generation. It is out of scope
    for the current loader and would be a separate research effort, not a
    refactor.
15. **64 KiB page readiness.** Only if devices require it; the 16 KiB build is
    compatible with 4 KiB and 16 KiB today.
16. **Other ABIs.** armeabi-v7a/x86_64 are not targets; adding them is mostly
    build/packaging work but multiplies the test matrix.
17. **H3VR2 integration.** Explicitly out of core scope. If pursued, it must
    be a separate project using the public mod ABI, with no H3VR2 identifiers
    in Mocha core, and it is not a multiplayer mod.

## Definition of done for "Quest supported"

Met for the test player on Quest 3 (2026-11-20):

- a Quest-hardware artifact exercising the L2 and L3 assertions —
  `artifacts/quest-external-pass-20261120-181426/` and
  `artifacts/quest-20261120-181220/`;
- no reliance on emulator translation for the loader's ELF and relocation
  behavior — gate, linker denial, staging and managed calls observed on the
  device's ARM64 CPU;
- documented OS version, page size, namespace policy, and install path —
  `docs/testing/results-quest-2026-11-20.md`;
- a statement of what was not tested (specific titles, other models/OS
  releases, immersive runtime) — §6 of that document;
- `docs/testing/results-quest-2026-11-20.md` added and the other docs updated.

The correct description is now: "Android/ARM64 loader verified on a physical
Quest 3 (Android 14, 4 KiB pages, non-root) with a minimal Unity 6000.6.9f1
test player, and on a 16 KiB-page emulator with ARM translation." It is not a
statement that every Quest title or OS release is supported.
