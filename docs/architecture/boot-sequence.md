# Boot Sequence

This document follows one process from `System.loadLibrary("main")` to the first
mod entry point and back to the engine. Line references are to the sources at
the time of writing.

## 1. Timeline

```
 t0  Android maps lib/<abi>/libmain.so          (Mocha's shim)
 t1  JNI_OnLoad                                  shim: load libmocha, bootstrap, register natives
 t2  Java calls NativeLoader.load(dir)
 t3  shim dlopen libunity.so                     RTLD_LAZY
 t4  shim notifies "libunity.so"                 gate installs on libunity
 t5  shim calls libunity JNI_OnLoad
 t6  shim dlopen libil2cpp.so                    RTLD_LAZY
 t7  shim notifies "libil2cpp.so"                loader stores handle
 t8  shim calls libil2cpp JNI_OnLoad
 t9  libunity resolves il2cpp_init               gate returns Mocha wrapper
 t10 wrapper calls real il2cpp_init              domain created
 t11 wrapper marks init complete                 ready callback fires (same thread)
 t12 run_mod_loading                             discover, order, load, invoke
 t13 wrapper returns to libunity                 engine continues, mods live
 ...
 tN  JNI_OnUnload                                mods shut down in reverse order
```

## 2. Detail

### t1 — `JNI_OnLoad` (`libmain_shim.cpp:170`)

1. `load_mocha()` (`:37`) opens `libmocha.so` with `RTLD_NOW | RTLD_GLOBAL`.
   `RTLD_NOW` means a missing loader symbol fails here, not later. If the bare
   soname is not found, `dladdr` on the shim's own `JNI_OnLoad` provides the
   directory and the sibling path is used.
2. The three loader entry points are resolved: `mocha_bootstrap`,
   `mocha_notify_library_loaded`, `mocha_shutdown`.
3. `mocha_bootstrap(vm)` runs (`loader.cpp:502`): resolves the app's data and
   internal directories through JNI, creates `mocha/`, `mocha/mods/` and
   `mocha/native/`, initializes file logging, and calls
   `set_runtime_ready_callback(&run_mod_loading)`.
4. `GetEnv` is called with `JNI_VERSION_1_6`; if the VM cannot supply it the shim
   fails rather than run with an unknown JNI contract.
5. `FindClass("com/unity3d/player/NativeLoader")` and `RegisterNatives` bind
   `load (Ljava/lang/String;)Z` and `unload ()Z`.
6. Logs `libmain shim ready` and returns `JNI_VERSION_1_6`.

Note that no Unity library has been loaded yet. The bootstrap has to work using
only libc and JNI, which is why it does not call any IL2CPP function.

### t2–t5 — `NativeLoader.load` and `libunity.so`

`native_load` (`:117`) receives the native library directory. It:

1. Logs `loading Unity libraries from <dir>`.
2. `dlopen(<dir>/libunity.so, RTLD_LAZY)` (`open_unity_library`, `:75`), with a
   bare-name fallback if the full path fails.
3. Calls `mocha_notify_library_loaded("libunity.so", handle)` **before**
   invoking that library's `JNI_OnLoad`.
4. Calls `JNI_OnLoad` on `libunity.so` and checks its return value.

Step 3 before step 4 is the critical ordering. `libunity.so`'s own `JNI_OnLoad`
may start threads or resolve imports; installing the gate first guarantees that
when the player eventually looks up `il2cpp_init`, the slot is already Mocha's.

`mocha_notify_library_loaded` (`loader.cpp:543`) dispatches on the name. For
`libunity.so` it calls `install_runtime_gate` (`runtime_gate.cpp:299`).

### Gate installation (`runtime_gate.cpp:299`)

1. `dl_iterate_phdr` finds the loaded object whose name is `libunity.so`
   (`find_unity`, `:77`) and records its load base.
2. The `PT_DYNAMIC` segment is walked for `DT_SYMTAB`, `DT_STRTAB`, `DT_STRSZ`,
   `DT_JMPREL`, `DT_PLTRELSZ`, `DT_PLTREL`, `DT_RELA`, `DT_RELASZ`, `DT_REL`,
   `DT_RELSZ`.
3. `DT_JMPREL` is patched first (`patch_rela_table`, `:247`), then `DT_RELA`
   and `DT_REL` (`:272`) for libraries that do not use the PLT.
4. A relocation is a target only if its type is `R_AARCH64_JUMP_SLOT` (1026) or
   `R_AARCH64_GLOB_DAT` (1025) **and** its symbol name is exactly `dlsym`.
5. `replace_slot` (`:121`) reads the page protection from `/proc/self/maps`,
   `mprotect`s to writable, writes `mocha_dlsym`, restores protection and calls
   `__builtin___clear_cache`.
6. The original `dlsym` is remembered so the wrapper can still call it.
7. Logs `runtime gate installed on libunity.so (dlsym slot, base=<base>)`.

If no `libunity.so` is found, or no `dlsym` relocation exists, the gate records
an error and reports failure. `run_mod_loading` later refuses to proceed when
the gate is not installed, so the failure is loud rather than silent.

### t6–t8 — `libil2cpp.so`

The shim opens `libil2cpp.so` with `RTLD_LAZY`, notifies the loader (which
stores the handle for later `dlsym` of the API), then calls its `JNI_OnLoad`.
Loading `libil2cpp.so` is what makes the `il2cpp_*` exports resolvable; the
loader does not call them until the domain exists.

### t9–t11 — interception and completion

`mocha_dlsym` (`:205`) calls the real `dlsym`. If the symbol is `il2cpp_init` or
`il2cpp_init_utf16`, it returns its own wrapper (`:175`, `:190`) and logs
`intercepted libunity il2cpp_init lookup` (and the `_utf16` variant).

The wrapper:

1. Calls the real function with the original argument.
2. `mark_init_complete` (`:147`) records the returned domain, sets
   `g_init_complete`, notifies the condition variable, and — exactly once —
   invokes `g_ready_callback` on the current thread.
3. Logs `il2cpp_init returned the IL2CPP domain (<domain>)`,
   `running mod initialization on the il2cpp_init thread`, and after the
   callback returns, `mod initialization returned to the engine`.

The callback runs synchronously and before the wrapper returns, so mods are
initialized while the engine is still in its own init sequence. This is
deliberate: see ADR 0003 for the `persistentDataPath` hang that motivated it.

### t12 — `run_mod_loading` (`loader.cpp:335`)

In order:

1. `wait_for_runtime` (`:196`): confirms the gate is installed, waits for
   `g_init_complete`, then polls `il2cpp_domain_get` and the assembly count every
   100 ms until both are available or the timeout expires.
2. Resolves every function in `MochaIl2CppApi` by name from `libil2cpp.so`.
   A missing function fails the API table rather than crashing.
3. Logs `<n> assemblies visible`.
4. `il2cpp_thread_attach(domain)`.
5. Resolves the Unity version via `UnityEngine.CoreModule.dll` →
   `UnityEngine.Application.unityVersion` (`try_resolve_unity_version`, `:168`).
6. Fills `g_public_api` (ABI version, struct size, loader version, Unity
   version, ABI name, API level, data dir, mods dir, log function, IL2CPP table).
7. `discover_mods` (`:275`): creates the mods directory, lists `*.json` sorted
   lexicographically, parses each, sets `library_path` to the sibling `.so`, and
   rejects entries whose library is missing.
8. `resolve_mods` (plan): validates, removes duplicates, disables unsatisfied
   and missing dependencies, and computes the order.
9. For each mod in plan order:
    - `open_mod_library` (`:228`): `dlopen` the sibling path; on failure copy to
      `<internal>/mocha/native/<stem>.so`, `chmod 0755`, `dlopen` the copy, and
      log `mod <id> loaded from internal copy`.
    - Resolve the entry symbol (default `mocha_mod_init`).
    - Call it inside a try/catch; a non-zero return or an exception marks the
      mod `failed:init`.
    - If the mod exports `mocha_mod_runtime_ready`, call it after a successful
      init.
    - If it exports `mocha_mod_shutdown`, remember it for shutdown.
    - Append `mod.<id>=loaded` or `mod.<id>=failed:<reason>` to the summary.
10. Writes `mocha/loader-summary.txt` and logs
    `mod load complete: discovered=<n> loaded=<n> failed=<n> skipped=<n>`.

### t13 — return

The wrapper returns the domain to `libunity.so`. The Unity player continues its
own startup and eventually runs the managed `Start`/`Update` loop. Mods are
already resident and their `mocha_mod_init` has run; anything they registered
lives in the process.

## 3. Shutdown

`JNI_OnUnload` (`libmain_shim.cpp:205`) calls `mocha_shutdown`
(`loader.cpp:572`), which:

1. Calls each remembered shutdown function in reverse load order.
2. Logs `Mocha shutdown complete`.
3. Closes the log file.
4. The shim then `dlclose`s `libmocha.so`.

Mods are not `dlclose`d individually. Android's linker does not guarantee
unloading a library that has live references, and the process is terminating, so
calling shutdown functions is the meaningful action.

## 4. Failure modes by phase

| Phase | Failure | Behaviour |
|---|---|---|
| t1 | `libmocha.so` missing | `JNI_OnLoad` fails; no native methods registered; Unity cannot start. Logged. |
| t1 | VM older than JNI 1.6 | Shim aborts; no partial registration. |
| t1 | `NativeLoader` class not found | Registration skipped with an error; Unity's call will fail. |
| t4 | `libunity.so` not found by `dl_iterate_phdr` | Gate error; mods refuse to run. |
| t4 | No `dlsym` relocation | Gate error; mods refuse to run. |
| t4 | `mprotect` fails | Slot not written; gate error; mods refuse to run. |
| t9 | Player resolves `il2cpp_init` before the gate | Not possible in this design: the gate is installed before `libunity.so`'s `JNI_OnLoad`, and the lookup happens after. |
| t11 | `il2cpp_init` fails | The wrapper still marks completion with a null domain; `wait_for_runtime` times out and mods do not run. |
| t12 | Mods directory unreadable | Empty discovery; logged; engine continues. |
| t12 | Manifest invalid | Mod rejected or skipped with a reason; others continue. |
| t12 | `dlopen` fails | Mod marked failed; others continue. |
| t12 | Entry missing | Mod marked failed; others continue. |
| t12 | Entry returns non-zero | Mod marked failed; others continue. |
| t12 | Entry throws C++ exception | Caught; mod marked failed; others continue. |

The last group is what "failure isolation" means in practice, and it is
exercised by `failing-mod` and `missing-dep-mod` in the artifact run.
