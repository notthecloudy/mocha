# Loader Lifecycle

This document describes the loader's states, per-mod states, threading, and
error handling. It complements `boot-sequence.md`, which is the chronological
view; this one is the state-machine view.

## 1. Loader states

The loader has one global lifecycle, guarded by atomics and two mutexes:

| State | Entered by | Meaning |
|---|---|---|
| `UNINITIALIZED` | process start | Nothing has run. |
| `BOOTSTRAPPED` | first `mocha_bootstrap` | Data/mods dirs exist, logging is live, the ready callback is registered, JNI natives are registered by the shim. |
| `GATE_ARMED` | `mocha_notify_library_loaded("libunity.so", …)` succeeds | The `dlsym` slot in `libunity.so` points at Mocha. `il2cpp_init` will be observed. |
| `RUNTIME_WAITING` | `libil2cpp.so` reported and its `JNI_OnLoad` returned | The API can be resolved, but the domain does not exist yet. |
| `RUNTIME_READY` | gate wrapper sees real `il2cpp_init` return | `il2cpp_domain_get` works; the ready callback fires. |
| `MODS_LOADING` | `run_mod_loading` passed `wait_for_runtime` | Discovery, planning and per-mod init. |
| `MODS_LOADED` | summary written | Steady state for the rest of the process. |
| `STOPPING` | `mocha_shutdown` first call | No further mods are started; shutdown callbacks run. |
| `STOPPED` | `mocha_shutdown` returns | Log closed. |

Transitions that are guarded:

- `mocha_bootstrap` uses `g_started.exchange(true)`. A second call returns
  immediately, so a player that calls `JNI_OnLoad` twice cannot reinitialize the
  loader or double-register natives.
- `set_runtime_ready_callback` can be called once per bootstrap; the gate
  invokes the callback at most once (`std::atomic<void(*)()>` swapped to null
  before the call).
- `mocha_shutdown` uses `g_stopping.exchange(true)`. A second call returns, so
  shutdown callbacks run once.
- `g_il2cpp_handle` and `g_loaded` are protected by mutexes because the shim
  thread and the shutdown path can run at different times.

### The one non-obvious transition

`RUNTIME_READY → MODS_LOADING` runs *inside* the `il2cpp_init` wrapper, before
the wrapper returns to `libunity.so`. That means the loader is re-entering
engine startup code while the engine is waiting for `il2cpp_init` to finish.
Anything a mod does during `mocha_mod_init` may touch a player that is only
partially initialized. This is the deliberate trade-off from ADR 0003: mods run
early enough to matter, on the correct thread, with a valid domain, but not
before Unity's managed startup has run. Mods that need to observe Unity's first
frame use `mocha_mod_runtime_ready`, which still runs early; a future
`on_scene_loaded` style callback would need engine hooks and is not implemented.

## 2. Runtime readiness

`wait_for_runtime` (`loader.cpp:196`) does three things in order:

1. Refuses unless `runtime_gate_installed()`. If the gate failed, calling
   `il2cpp_*` would be unsafe, so mods are not started.
2. Waits up to 60 seconds (`kRuntimeTimeoutSeconds`) for `il2cpp_init` to be
   observed. Timeout means the player never called the symbol through
   `libunity.so`'s `dlsym`; the loader logs and stops.
3. Polls `il2cpp_domain_get` and `il2cpp_domain_get_assemblies` every 100 ms
   until both return non-null/non-zero, with the same deadline. This covers the
   window where `il2cpp_init` returned but assembly registration is still in
   progress.

A timeout is a clean failure: no mod is started, and the engine continues
without mods. The process is not crashed by the loader.

## 3. Per-mod lifecycle

Each manifest moves through a subset of these states:

| State | Cause | Visible as |
|---|---|---|
| `DISCOVERED` | `*.json` parsed and sibling `.so` exists | counted in `discovered=` |
| `REJECTED` | parse error, bad schema/id/ABI, missing library | `rejected=<reason>`, not counted in `discovered` |
| `SKIPPED` | invalid version, duplicate id, missing/disabled/unsatisfied dependency, cycle | `skipped_mod=<id>: <reason>`, counted in `skipped=` |
| `DLOPEN_FAILED` | neither the sibling nor the internal copy loads | `mod.<id>=failed:dlopen` |
| `ENTRY_MISSING` | manifest's entry symbol not exported | `mod.<id>=failed:entry-missing` |
| `INIT_FAILED` | entry returned non-zero or threw | `mod.<id>=failed:init` |
| `LOADED` | entry returned 0 | `mod.<id>=loaded` |
| `SHUT_DOWN` | shutdown symbol called during `mocha_shutdown` | log only |

Notes:

- `REJECTED` mods are removed before counting; `SKIPPED` mods are part of the
  discovered set but not loaded. This is why the artifact shows
  `discovered=4 skipped=1` for four manifests where one has a missing
  dependency.
- A mod that fails to load is *not* retried. There is no partial state and no
  rollback beyond not calling shutdown for mods that never initialized.
- `mocha_mod_runtime_ready` is optional. If it returns non-zero or throws, the
  mod stays `LOADED`; the hook is advisory.
- `mocha_mod_shutdown` is optional. If absent, nothing is called at shutdown.
- Shutdown is called in reverse load order, so a mod that initialized later
  (and may depend on an earlier one) tears down first.

## 4. Threading

| Work | Thread |
|---|---|
| `JNI_OnLoad`, registration, bootstrap | JVM thread that loads `libmain.so` |
| `NativeLoader.load`, `dlopen` of Unity libs | Unity player's native load thread |
| Gate install | same thread, before `libunity.so`'s `JNI_OnLoad` |
| `il2cpp_init` wrapper and `run_mod_loading` | the thread that called `il2cpp_init` |
| Mod entry points | same thread as above (synchronous) |
| Mod shutdown | whatever thread calls `JNI_OnUnload` |

Because mod init is synchronous on the engine's init thread, there is no
loader-side concurrency during `run_mod_loading`: it is single-threaded by
construction. The mutexes exist for the boundaries (handle report from the shim,
shutdown from another thread), not for parallel mod loading.

`il2cpp_thread_attach(domain)` is still called before mods. The thread is
already attached in practice, but making it explicit documents the contract and
covers a player that calls `il2cpp_init` from a thread it has not attached.

Mods that create threads of their own must attach them; the loader cannot do
that for them. This is stated in the SDK header and in
`docs/research/il2cpp-interop.md` §6.

## 5. Error model

The loader distinguishes:

- **Fatal to the loader**: cannot obtain `JNIEnv`, cannot resolve the data
  directory, cannot resolve the IL2CPP API. Logged; mods do not run; engine
  continues.
- **Fatal to mod loading**: gate not installed, `il2cpp_init` timeout,
  assemblies never appear. Logged; no mod runs.
- **Fatal to one mod**: everything in the per-mod table above. Logged; other
  mods continue.
- **Advisory**: missing optional IL2CPP symbols, runtime-ready hook error,
  mod thread attach failure. Logged as warnings; loading continues.

No path calls `abort()`, `exit()`, or throws across the shim/loader boundary.
The shim's `JNI_OnLoad` returning a failure is the only way to refuse to load,
and it does so before registering natives, so the player sees a clean failure
rather than a half-initialized shim.

## 6. Summary file

`<data>/mocha/loader-summary.txt` is written after the loading loop. It is
line-oriented `key=value`, written in this order:

```
loader_version=0.1.0
abi=arm64-v8a
unity=6000.6.9f1
discovered=4
skipped=1
rejected=<reason>            (zero or more)
skipped_mod=<id>: <reason>   (zero or more)
mod.<id>=failed:dlopen       (per failed/dloaded mod, in load order)
mod.<id>=failed:entry-missing
mod.<id>=failed:init
mod.<id>=loaded
loaded=2
failed=1
```

The file is truncated on each run, so it reflects the current process only. It
is the machine-readable contract used by `scripts/run_emulator_test.sh`, which
asserts exact counts and the presence of `loaded`/`failed` entries. The
captured example is `artifacts/emulator-20261118-191549/loader-summary.txt`.

If the loader stops before the loop (gate/runtime failure), no summary is
written. A stale summary from a previous run is therefore possible; the test
harness deletes it before launching.

## 7. Shutdown

`mocha_shutdown`:

1. Atomically marks `STOPPING`; a concurrent or repeated call returns.
2. Copies the loaded list under a mutex.
3. Calls each shutdown function in reverse order inside a try/catch. A throwing
   mod logs `mod shutdown raised an exception` and does not stop the others.
4. Logs `Mocha shutdown complete` and closes the file log.

The shim then `dlclose`s `libmocha.so`. Mod handles are intentionally not
`dlclose`d: the process is exiting, and Android does not guarantee safe unload
of a library with live references.
