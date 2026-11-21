# ADR 0003: Run mod initialization on the `il2cpp_init` thread

- Status: accepted, implemented; replaces the earlier worker-thread design
- Date: 2026-11-19
- Code: `native/src/loader/runtime_gate.cpp:147-173`,
  `native/src/loader/loader.cpp:335-497, 502-541`
- Related: ADR 0002, `docs/specifications/mod-abi.md` §6

## Context

Once the gate has observed `il2cpp_init` return a domain, Mocha has to resolve
the IL2CPP API, attach the calling thread, and invoke each mod's entry point.
Two properties matter:

1. **Validity.** IL2CPP's API is only usable with a live domain, and threads
   must be registered with it. Calling `domain_get` before initialization, or
   calling any API from a thread never attached to the domain, is outside the
   supported contract.
2. **Timing.** Mods want to see as much of the engine as possible while still
   being present for the player's normal lifecycle. Running before IL2CPP
   starts is impossible without a domain; running arbitrarily late means
   missing early game setup.

The first implementation in this repository used a loader-owned worker: the
gate signalled a condition variable, and the worker waited, polled
`il2cpp_domain_get` and `il2cpp_domain_get_assemblies` every 100 ms, then
attached itself with `il2cpp_thread_attach` and loaded mods. That works, and
captured runs from 18:48–19:12 in `artifacts/` show it completing. It has
drawbacks that are structural rather than incidental:

- Two threads observe "runtime ready" at different instants: the engine's init
  thread returned from `il2cpp_init` and continued startup while the worker was
  still polling and attaching. Mods therefore interleave with Unity's continued
  initialization without any ordering guarantee.
- The worker's attachment is a repair for a thread that the engine did not
  create for this purpose; any engine call made by a mod runs on a foreign
  thread while the engine's own init thread is mid-startup.
- Detecting "ready" by polling cannot distinguish "not yet" from "this symbol
  is unavailable"; a timeout is the only conclusion.
- Mod ordering relative to the engine is timing-dependent, not defined.

## Decision

`run_mod_loading` is registered as the runtime-ready callback and is invoked
**synchronously, once, by the gate wrapper on the thread that called
`il2cpp_init`**, immediately after the real `il2cpp_init` (or
`il2cpp_init_utf16`) returns a non-null domain:

```
engine thread: dlsym("il2cpp_init") → mocha_il2cpp_init
  real il2cpp_init(argument) → domain
  mark_init_complete(domain)
    set g_init_complete; notify waiters
    if callback registered and not yet run:
        callback()            ← run_mod_loading: API resolution, attach,
                                 discovery, plan, mod entries
  return domain to libunity
```

The callback runs after the domain exists and on a thread that is already part
of the engine's initialization, so `il2cpp_domain_get`,
`il2cpp_domain_get_assemblies`, `il2cpp_thread_attach` and every mod's
`il2cpp_*` call use the sanctioned thread. Registration happens in
`mocha_bootstrap`; the gate uses an atomic callback pointer and an atomic
"already ran" flag, so a second init path or a retry cannot run mods twice.

`wait_for_runtime` still exists and still waits on the gate condition
(`wait_for_il2cpp_init`) and polls for assemblies. In the callback path the
condition is already satisfied, so the wait returns immediately and the poll
loop only covers a player that creates the domain fractionally before
assemblies are registered. The 60-second timeout remains as a fail-closed
guard, and mods never start if the gate is not installed.

Mod entry points are run synchronously, in plan order, on that same thread. A
mod that blocks delays Unity startup; this is stated in the ABI documentation
and is the deliberate cost of deterministic timing. Mods that want to defer
work can use `mocha_mod_runtime_ready` (currently the same moment) or create
their own thread and attach it.

## Consequences

Positive:

- One well-defined point of "runtime ready", observed on the correct thread.
- No poll/attach race with engine startup, and no reliance on
  `il2cpp_thread_attach` to make a foreign thread legal for the first call.
- Mods execute before Unity proceeds to script-engine setup in the captured
  run: the log shows `running mod initialization on the il2cpp_init thread`,
  the mod results, `mod initialization returned to the engine`, then Unity's
  `CodeReloadManager initialized` / `InitializeScriptEngine OK`. That ordering
  is a property of the design, not a coincidence of scheduling.
- The domain and assembly list are guaranteed non-null before any mod runs, so
  a mod does not have to retry or tolerate a half-created runtime.

Negative and risks:

- **Startup latency is now in the engine's critical path.** Discovery, JSON
  parsing, `dlopen`, and every mod's init run while `il2cpp_init` has not
  returned. The captured run takes single-digit milliseconds for four mods;
  a slow mod is a slow game boot. There is no watchdog and no timeout around a
  mod entry.
- **Partial engine state.** The domain exists, but Unity's managed startup and
  scene loading have not completed. A mod that assumes a scene, a player
  object, or a fully initialized Unity API may fail. This is documented as a
  hard rule in the ABI spec; the loader cannot detect it.
- **No later hook.** There is no `on_scene_loaded` callback. Anything needing
  a later point must use a mod-owned thread or poll from the mod.
- If a future player initializes IL2CPP from a thread that should not run
  arbitrary code, or calls init more than once with different domains, the
  callback model does not retry. `g_ready_ran` makes it once-only; only the
  first successful init is used.

## Alternatives considered

- **Worker thread polling (previous implementation)**: works and is in the
  artifacts, but has the ordering and thread-affinity weaknesses above.
  Rejected for the final design; the wait code remains as a guard.
- **Load mods in `NativeLoader.load` after `libil2cpp.so`'s `JNI_OnLoad`**:
  at that point `il2cpp_init` has not run in the tested player, so there is no
  domain. It would still need polling, with the same drawbacks and even less
  information.
- **Hook the Unity player loop or a scene-load callback**: would give a later,
  safer point, but requires engine-internal hooks that are version-specific
  and would violate the no-game-specific-hooks rule. Not implemented.
- **Run mods from `JNI_OnLoad` of `libmocha.so`**: same problem as above; no
  domain exists and Unity has not started.

## Evidence

- `native/src/loader/runtime_gate.cpp:147-173` (`mark_init_complete`, callback
  invoked after `g_init_complete` is set), `:175-203` (both init wrappers),
  `:402-409` (callback registration and wait).
- `native/src/loader/loader.cpp:502-541` (bootstrap registers
  `run_mod_loading`; comment states the intent), `:335-372` (API resolution,
  runtime wait, assembly count, thread attach), `:418-488` (synchronous mod
  loop).
- Captured final run `artifacts/emulator-20261118-191549/logcat.txt:799-824`:
  `il2cpp_init returned the IL2CPP domain (0x1)` and
  `running mod initialization on the il2cpp_init thread` on thread 23719,
  mod results, `mod initialization returned to the engine`, then Unity's
  script-engine messages on the same thread.
- Earlier worker-thread runs, e.g.
  `artifacts/emulator-20261118-190445/logcat.txt:774-779`, show the mod work
  on a different thread (14097) from the one that returned from init (14101),
  which is the behavior this ADR replaces.

## Non-claims

- The captured evidence was originally from an x86_64 API 37 emulator running
  the arm64-v8a build under Berberis translation, not Quest hardware. Thread
  scheduling and engine order on Quest were unverified at that point.
- No claim is made that running during `il2cpp_init` is safe for arbitrary
  Unity APIs; only domain/assembly/thread operations and the mod interactions
  exercised by the test mods were verified.

Update (2026-11-20): the same-thread ordering was observed on a physical Meta
Quest 3 (Android 14, 4 KiB pages): `il2cpp_init` returned the domain, the mods
ran and wrote their results, and Unity then logged `InitializeScriptEngine OK`.
This closes the Quest ordering non-claim for the test player and that OS; see
`docs/testing/results-quest-2026-11-20.md`.
