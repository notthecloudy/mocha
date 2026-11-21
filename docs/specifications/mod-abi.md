# Mod ABI

Status: version 1, implemented. Header: `sdk/include/mocha/mocha_api.h`.
Loader fill-in: `native/src/loader/loader.cpp:379`. Symbol resolution:
`native/src/loader/il2cpp_api.cpp`.

This is the contract a native mod compiles against. It is pure C: no C++
types, no Android headers, no Unity headers.

## 1. What the ABI guarantees

- The loader passes a `const MochaApi*` to the entry point.
- `MochaApi` and `MochaIl2CppApi` are **append-only**. Existing fields never
  move, change type or change meaning.
- `MOCHA_ABI_VERSION` (currently `1`) changes only for an incompatible change,
  and the manifest `abi` field must equal it before any mod is loaded.
- The API table is fully populated before the first entry point is called and
  stays valid until shutdown.
- Each mod is called on the thread that ran `il2cpp_init`, in resolution order,
  and gets a domain-attached thread.

## 2. Exported symbols

A mod is a shared library exporting C symbols with default visibility.

| Symbol | Required | Signature | When |
|---|---|---|---|
| entry (default `mocha_mod_init`) | Yes | `int (*)(const MochaApi*)` | Load order, once. Non-zero means failed. |
| `mocha_mod_runtime_ready` | No | `int (*)(const MochaApi*)` | Immediately after a successful entry, same thread, once. Non-zero only warns. |
| `mocha_mod_shutdown` | No | `void (*)(void)` | Reverse load order during `mocha_shutdown`. Exceptions are caught and logged. |

Notes:

- The entry symbol name is per-manifest (`entry` field), defaulting to
  `mocha_mod_init`. The other two names are fixed and are looked up by the
  literal strings in the header (`MOCHA_DEFAULT_RUNTIME_READY`,
  `MOCHA_DEFAULT_SHUTDOWN`); they are not configurable in schema 1.
- Declaration macro:

  ```c
  MOCHA_MOD_EXPORT int mocha_mod_init(const MochaApi* api);
  ```

  In C++ this expands to `extern "C" __attribute__((visibility("default")))`.
- A symbol that is not exported (hidden, `static`, name-mangled) is reported as
  `failed:entry-missing` for the entry, or silently absent for the optional two.
- The shutdown function receives nothing. A mod that needs its `MochaApi` after
  init must store the pointer (it stays valid) or copy the values it needs.

## 3. `MochaApi`

Field order and LP64 offsets (verified by compiling the shipped header on an
LP64 host; `sizeof` is 72):

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `abi_version` | `uint32_t` | `MOCHA_ABI_VERSION` of the loader. |
| 4 | `struct_size` | `uint32_t` | `sizeof(MochaApi)` of the loader. |
| 8 | `mocha_version` | `const char*` | Loader version, e.g. `"0.1.0"`. |
| 16 | `unity_version` | `const char*` | Resolved engine version, or `NULL`. |
| 24 | `android_abi` | `const char*` | Build ABI of the loader: `"arm64-v8a"`, `"x86_64"`, … |
| 32 | `android_api_level` | `int32_t` | `ro.build.version.sdk`, or `0` if unreadable. |
| 40 | `data_directory` | `const char*` | Absolute Mocha data root. |
| 48 | `mods_directory` | `const char*` | Absolute mods directory. |
| 56 | `log` | `MochaLogFn` | Thread-safe logger; never `NULL`. |
| 64 | `il2cpp` | `const MochaIl2CppApi*` | Never `NULL`; individual functions may be. |

Strings are UTF-8, NUL-terminated, and owned by Mocha. The header tells mods to
copy any value they need after init returns; in this implementation the strings
and the struct are process-lifetime, but relying on that is outside the
contract.

`unity_version` is `NULL` when the loader could not resolve it (for example the
engine assembly is not named `UnityEngine.CoreModule.dll`, or the method was
stripped). Mods must handle `NULL`.

`log` is `void (*)(MochaLogLevel, const char*)` with levels
`TRACE..ERROR`. It writes to logcat under tag `Mocha` and to
`<data>/mocha/mocha.log`, serialized by a mutex, so it is safe from mod-created
threads. It does not prefix or split multi-line messages: if a mod logs a string
containing newlines, only the first line carries the timestamp and level. Log
one line per call.

## 4. `MochaIl2CppApi`

Field order and LP64 offsets (`sizeof` 224). The pointer is `NULL` for any
function the running `libil2cpp.so` did not export.

| Offset | Field | Offset | Field |
|---|---|---|---|
| 0 | `struct_size` | 120 | `class_get_field_from_name` |
| 8 | `domain_get` | 128 | `field_static_get_value` |
| 16 | `thread_attach` | 136 | `field_static_set_value` |
| 24 | `thread_detach` | 144 | `field_get_value` |
| 32 | `domain_get_assemblies` | 152 | `field_set_value` |
| 40 | `assembly_get_image` | 160 | `field_get_offset` |
| 48 | `class_from_name` | 168 | `image_get_name` |
| 56 | `class_get_method_from_name` | 176 | `class_get_name` |
| 64 | `runtime_invoke` | 184 | `class_get_namespace` |
| 72 | `string_new` | 192 | `method_get_name` |
| 80 | `string_chars` | 200 | `method_get_param_count` |
| 88 | `string_length` | 208 | `gc_disable` |
| 96 | `object_new` | 216 | `gc_enable` |
| 104 | `object_unbox` | | |
| 112 | `object_get_class` | | |

These are all the 27 functions in the table; there are no hidden fields. Do not
hand-declare this struct — include the header, so padding matches.

### Required subset

`resolve_il2cpp_api` fails, and no mods run, unless these seven resolve:

`domain_get`, `thread_attach`, `domain_get_assemblies`, `assembly_get_image`,
`class_from_name`, `class_get_method_from_name`, `runtime_invoke`.

Everything else is optional. The loader counts nulls and logs
`<n> optional IL2CPP symbols are unavailable` as a warning. A mod must check
every optional pointer it uses (`if (!api->il2cpp->string_new) …`) rather than
assuming presence. The SDK does not paper over this.

Resolution tries `dlsym(handle_of_libil2cpp, "il2cpp_<name>")` first, then
`dlsym(RTLD_DEFAULT, …)`. Function pointers correspond exactly to the exported
C API; no trampolines or wrappers are inserted.

### Semantics

The semantics of each function are IL2CPP's, not Mocha's:
parameters are addresses, returns are boxed, exceptions are returned through
`void** exception_out`, and strings are UTF-16. The practical rules are in
`docs/research/il2cpp-interop.md` §3–§6. The ABI only fixes names and types; it
cannot validate a mod's pointer usage.

Note that `runtime_invoke`'s `exception_out` is `void**` here rather than
`Il2CppException**`; the pointer value is the same. `object_new` takes
`void* klass` (the header comment says `const Il2CppClass*` for IL2CPP; the
cast is representation-identical).

## 5. Versioning rules

Two independent numbers:

- **`MOCHA_ABI_VERSION` / manifest `abi`**: incompatible contract version. A
  mismatch is rejected before `dlopen`, so a mod never runs against a layout it
  was not built for.
- **`struct_size`**: exact size of the struct the producer built with. Lets
  either side detect which fields exist.

Rules for changes:

1. Adding fields at the end of either struct, with no change to existing
   offsets or meanings, is compatible. `struct_size` grows; `MOCHA_ABI_VERSION`
   stays.
2. Reordering, removing, retyping, or changing the meaning of a field requires a
   new `MOCHA_ABI_VERSION`.
3. Adding a *new optional* symbol, changing entry semantics, or changing
   failure/reporting semantics is a compatibility question decided per change;
   when in doubt, bump the version.
4. Old loader + new mod: the mod's manifest `abi` must still match, so the mod
   must have been built with the loader's ABI version. Its `struct_size`-based
   reads protect it from reading fields an older loader did not fill.

The safe reader pattern, which the header states and this document repeats:

```c
if (api->struct_size >= offsetof(MochaApi, some_new_field) + sizeof(api->some_new_field)) {
    /* field is present */
}
```

`offsetof` on a field that does not exist in the mod's own header will not
compile, so a mod can only test fields it knows about; the check is for a loader
older than the mod, not newer.

## 6. Lifetimes and threading

| Object | Lifetime |
|---|---|
| `MochaApi` and strings | Created before the first entry; stable through shutdown. Copy if you need persistence beyond the contract. |
| `MochaIl2CppApi` | Same; written once by `resolve_il2cpp_api`, read-only afterwards. |
| IL2CPP objects (`Il2CppObject*`, strings, boxed values) | Governed by IL2CPP and the GC; not by Mocha. |
| Entry/runtime-ready calls | Same thread as `il2cpp_init`, synchronous, in plan order. |
| Shutdown calls | Reverse order, on the `JNI_OnUnload` thread. |

Mocha attaches the init thread to the domain with `il2cpp_thread_attach`. Any
thread a mod creates must attach itself; the loader cannot do it. Do not assume
that IL2CPP calls are safe on an unattached thread — they are not.

The init thread is inside engine startup when mods run (ADR 0003), so a mod that
blocks delays Unity startup. There is no watchdog and no timeout around a mod's
entry; a hang is a hang. Do work that must not block in `runtime_ready` or on a
mod-owned thread, and return promptly from init.

## 7. Calling convention and toolchain

- C calling convention (`extern "C"`), default ABI for the target. No
  `stdcall`/`fastcall` variants.
- Build with the NDK for the ABI the loader was built for (`arm64-v8a` for the
  Quest target). A 32-bit mod cannot be loaded by a 64-bit process.
- `-shared -fPIC`, and link the C++ runtime statically (`c++_static`) if you use
  C++. Do not require `libc++_shared.so` unless you also ship it; the loader
  does not add a C++ runtime to the process.
- No STL containers, `std::string`, or C++ objects may cross the boundary. The
  ABI is C structs and pointers.
- Exceptions must not escape an exported function. The loader wraps entry and
  runtime-ready in `try/catch` as a last resort and records `failed:init`, but
  the header's rule is that the mod handles its own errors; unwinding through a
  `dlsym`-called C boundary is undefined behavior.
- Do not rely on RTTI/`typeinfo` matching across the boundary.
- Mods must not call `exit()`/`abort()`; that kills the player, and there is no
  process isolation.

## 8. Failure semantics

- Entry returns `0`: loaded. Any other value (including negative): `failed:init`
  with the value logged.
- Entry throws: caught, logged, `failed:init`.
- Missing entry symbol: `failed:entry-missing`.
- `dlopen` fails both from the mods directory and from the internal copy:
  `failed:dlopen`.
- Runtime-ready returns non-zero or throws: warning only; the mod stays loaded.
- Shutdown throws: caught; remaining shutdowns still run.

A failed mod does not stop the loop and does not roll back anything. If a mod
needs cleanup on partial initialization, it must do that itself before returning
non-zero.

## 9. Checklist for a mod author

1. Include `mocha_api.h`; do not copy the structs.
2. Export the entry with `MOCHA_MOD_EXPORT extern "C"`.
3. Return non-zero on any failure you cannot recover from; log why.
4. Check `api->unity_version` for `NULL`.
5. Check every `il2cpp` function pointer for `NULL` before use.
6. Never let an exception cross the exported functions.
7. Attach any thread you create; detach it before it exits.
8. Copy API strings if you need them after shutdown begins.
9. Build for `arm64-v8a` with the same ABI version the loader reports.
10. Keep init fast; the engine is waiting.

## 10. Non-guarantees

- The ABI does not sandbox mods. A mod runs with the app's privileges and can
  crash the process.
- It does not guarantee that a type, method or field exists in a given player;
  AOT stripping can remove them. Resolve defensively.
- It does not expose managed assembly loading, Reflection.Emit, or a CLR. Mods
  are native code (ADR 0005).
- It does not provide overload selection by type, generic instantiation helpers,
  or GC handles. Those are IL2CPP limitations, not omissions that the loader
  can fix.
- No claim is made about ABI compatibility with any future Unity version. Only
  the IL2CPP C API as exported by the tested player (Unity 6000.6.9f1) was
  exercised.
