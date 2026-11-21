# ADR 0005: Mods are native libraries behind a C ABI, not managed assemblies

- Status: accepted, implemented
- Date: 2026-11-19
- Code: `sdk/include/mocha/mocha_api.h`,
  `native/src/loader/il2cpp_api.cpp`, `native/src/loader/loader.cpp:335-497`
- Related: `docs/specifications/mod-abi.md`,
  `docs/research/unity-il2cpp-loading.md`, `docs/research/il2cpp-interop.md`

## Context

The most familiar modding model on PC is "drop a `.dll` in a folder". BepInEx,
MelonLoader and UnityDoorstop all present that experience, and it is what a
C# mod author expects. It is not what an IL2CPP player can do by itself.

A Unity IL2CPP build converts every managed assembly to C++ and compiles the
result into `libil2cpp.so`. The player ships **no managed execution engine**.
The `.dll` files that may still be present are metadata carriers used by the
IL2CPP runtime for reflection and type resolution; they are not executable by
any CLR in the process. UnityDoorstop's own documentation states the
constraint and, consequently, hosts **CoreCLR** to run managed code.

So "load a managed assembly" is not a small feature. It requires:

- shipping a full managed runtime (CoreCLR or Mono) for Android/ARM64 in the
  process, initialized alongside Unity's own GC and threads;
- a generated interop layer that maps IL2CPP types, methods and generic
  instantiations into something the hosted runtime can call, regenerated for
  every player version (Il2CppInterop, Cpp2IL output);
- solving two-GC coexistence, thread attachment in both runtimes, and
  assembly-loading semantics;
- a large increase in binary size (tens of MB) and startup work.

That is what the established PC loaders do, and BepInEx still lists IL2CPP-on-
ARM as unsupported; MelonLoader's Android/Quest support is described as work
in progress. The problem is real, not an implementation detail.

Mocha's requirements are narrower: a genuinely working loader, generic across
Unity IL2CPP titles, with no game-specific assumptions, testable end to end,
and honest about what it does. It does not require C# mods.

## Decision

A Mocha mod is a **native shared library** (`.so`) for the player's ABI,
exporting C entry points and using a versioned C API table supplied by the
loader. Mocha does **not** embed a CLR, does **not** load managed assemblies,
and does **not** generate IL2CPP-to-managed proxies.

The contract (schema/ABI version 1):

- `int mocha_mod_init(const MochaApi* api)` (or a manifest-named entry), plus
  optional `mocha_mod_runtime_ready` and `mocha_mod_shutdown`.
- `MochaApi` carries version/size fields, the Unity version (or `NULL`), the
  ABI, API level, data/mods directories, a thread-safe logger, and a
  `MochaIl2CppApi` table.
- `MochaIl2CppApi` exposes **27** functions resolved with `dlsym` from the
  running `libil2cpp.so` (required subset of 7: `domain_get`,
  `thread_attach`, `domain_get_assemblies`, `assembly_get_image`,
  `class_from_name`, `class_get_method_from_name`, `runtime_invoke`).
- Structs are append-only; offsets and sizes are fixed; mods include the
  shipped header rather than redeclaring them.

Mods therefore do explicitly what a managed mod would do through reflection:
walk `domain → assemblies → images → classes → methods`, then call
`runtime_invoke`. The loader provides the handles and the calling convention;
it does not interpret C#.

This is a deliberate product choice, not a claim that native mods are better.
It keeps the loader small and the semantics observable, and it makes the
failure modes testable: a missing symbol is `NULL`, a missing method is
`NULL`, a mod entry error is a return code.

## Consequences

Positive:

- No CLR, no second GC, no generated interop layer, no per-version proxy
  regeneration. The loader is ~4.9 MB (`libmocha.so`) and starts in
  milliseconds.
- Works with the AOT player as it exists: the mod calls the same exported C
  functions Unity's generated code calls, on a thread attached to the domain.
- The API surface is auditable and stable; the 27 functions are the contract,
  and the header is the single source of truth.
- Three test mods compiled against the SDK loaded and ran in the emulator
  (one deliberately failing, one with a dependency), exercising entry,
  runtime-ready, logging and IL2CPP lookup.

Negative and risks:

- **Mod authors must write C/C++**, not C#. This excludes the majority of the
  existing Unity modding community and its tooling (Harmony, BepInEx
  plugins, IL weaving). It is the single biggest limitation of this design.
- No managed assembly loading, no Reflection.Emit, no C# generic
  instantiation helpers, no GC handles beyond what the C API offers.
- IL2CPP method discovery is name-and-arity based. Overloads with the same
  name and parameter count, generic methods, and stripped methods are hard or
  impossible to reach; the mod must handle `NULL`.
- A mod is native code in the game's process: a crash, `abort()`, or memory
  corruption takes down the player. There is no sandbox and no process
  isolation. The same is true of native mods in PC loaders, but it is worth
  stating.
- The C ABI is fixed at build time. A mod compiled against a different
  `MOCHA_ABI_VERSION` is rejected before `dlopen`; a newer loader adding fields
  is handled by `struct_size`, but removals are not.
- The mod's own dependencies must be resolvable by the player's linker
  namespace, which is why the loader stages libraries internally (ADR 0004).

## Alternatives considered

- **Host CoreCLR and load managed assemblies (UnityDoorstop/BepInEx-style)**:
  gives the expected authoring experience, but requires a large embedded
  runtime and a per-version IL2CPP interop layer. This is a project in itself,
  not a step in this one. Out of scope; documented as a possible future line in
  `docs/roadmap.md`, not implemented.
- **Embed Mono**: smaller than CoreCLR but still a second runtime, plus the
  same interop problem, and Mono's Android embedding is not a supported public
  surface for this use.
- **Generate C# proxies with Il2CppInterop and compile mods against them**:
  still needs a CLR to execute the proxies. Without one, the generated code is
  only useful to C++ callers. Not a substitute.
- **Embed a small scripting VM (Lua/WASM) and expose IL2CPP calls as host
  functions**: friendlier than C for some authors, but it is another runtime
  and another ABI to design, with no existing ecosystem for IL2CPP access.
  Rejected for scope.
- **Inline-patch IL2CPP methods from the loader and expose hooks only**: less
  flexible than an API table, and it would make the loader responsible for
  per-game method addresses. Rejected; mods own their own hooks with the tools
  they choose.
- **Managed `.dll` mods with a reflection shim that "just works"**: not
  possible in an AOT player without a CLR. Rejected as a false promise.

## Evidence

- `docs/research/unity-il2cpp-loading.md` §"IL2CPP is AOT" and §"Consequences":
  the player ships no managed execution engine; UnityDoorstop hosts CoreCLR
  because "Il2Cpp cannot run managed assemblies"; BepInEx IL2CPP ARM support
  and MelonLoader Android status are recorded there with dates.
- `docs/research/il2cpp-interop.md` §1: the C API is the stable surface; the
  tested player exported 243 `il2cpp_*` symbols and Mocha names 27.
- `sdk/include/mocha/mocha_api.h` and `docs/specifications/mod-abi.md`:
  the exact structs, offsets and symbols.
- Captured run `artifacts/emulator-20261118-191549`: `IL2CPP API resolved`,
  `IL2CPP runtime is ready (19 assemblies visible)`, and the three test mods'
  results (`hello` and `dependent` loaded, `failing` failed by design).

## Non-claims

- Mocha does not load managed assemblies and has no plan to claim it does.
- The native ABI was exercised only on Unity 6000.6.9f1, arm64-v8a. The first
  runs used an x86_64 emulator with ARM translation; a later run used a physical
  Quest 3 (Android 14). No other Unity version was tested.
- No claim that the 27-function table covers everything a mod might want; it
  is the subset needed for the tested scenarios and the functions the player
  exports.
