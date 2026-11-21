# IL2CPP Interop

Research date: 2026-11-19
Scope: how native code resolves and calls into a live Unity IL2CPP player, and
what Mocha exposes to mods. Signatures quoted below are copied from the Unity
6000.6.9f1 installation at
`~/Unity/Hub/Editor/6000.6.9f1/Editor/Data/il2cpp/libil2cpp/il2cpp-api-functions.h`.

## 1. Why the C API and not the C++ internals

IL2CPP compiles managed code to C++ and then to native code. The C++ classes
behind `Il2CppClass`, `MethodInfo` and friends are internal: their layout, name
mangling and virtual tables are build artifacts and change between Unity
versions and compiler settings. Linking against them is not supportable.

What is stable is the exported C API declared in `il2cpp-api.h`. It is a flat,
`extern "C"` surface intended for embedders and generated code, and the functions
take opaque pointers:

```c
typedef struct Il2CppDomain Il2CppDomain;
typedef struct Il2CppAssembly Il2CppAssembly;
typedef struct Il2CppImage Il2CppImage;
typedef struct Il2CppClass Il2CppClass;
typedef struct MethodInfo MethodInfo;
typedef struct FieldInfo FieldInfo;
typedef struct Il2CppObject Il2CppObject;
typedef struct Il2CppString Il2CppString;
typedef struct Il2CppException Il2CppException;
```

Mocha therefore resolves these functions by name at runtime and never includes a
Unity C++ header or links to `libil2cpp.so`. The test player exported 243
`il2cpp_*` symbols; Mocha names 27 of them.

## 2. Resolution sequence

The API is a graph walk. Every step needs the previous handle:

```
il2cpp_domain_get()
    → Il2CppDomain*
il2cpp_domain_get_assemblies(domain, &count)
    → const Il2CppAssembly*[count]
il2cpp_assembly_get_image(assembly)
    → const Il2CppImage*
il2cpp_class_from_name(image, namespace, name)
    → Il2CppClass*
il2cpp_class_get_method_from_name(klass, name, arg_count)
    → const MethodInfo*
```

Signatures, verified from the installed header:

```c
Il2CppDomain* il2cpp_domain_get(void);
const Il2CppAssembly** il2cpp_domain_get_assemblies(const Il2CppDomain* domain, size_t* size);
Il2CppClass* il2cpp_class_from_name(const Il2CppImage* image, const char* namespaze, const char* name);
const MethodInfo* il2cpp_class_get_method_from_name(Il2CppClass* klass, const char* name, int argsCount);
```

Two practical points:

- `il2cpp_class_from_name` requires the image, not just the class name. A mod
  must know which assembly defines the type. Game code normally lives in
  `Assembly-CSharp.dll` (the default assembly for scripts not under an asmdef);
  package and engine code live elsewhere. The test app follows that convention
  and the hello mod resolves `Assembly-CSharp.dll` explicitly.
- Overloads are selected by argument count, not by types. Two methods with the
  same name and the same arity cannot be distinguished through this function;
  the mod gets whichever the runtime considers the match. The SDK does not hide
  this limitation.

Namespace is `""` (empty string) for global-namespace types, not `NULL`. Mocha's
examples pass `"MochaTestApp"` because the test type is namespaced.

## 3. Invocation

The core call is:

```c
Il2CppObject* il2cpp_runtime_invoke(
    const MethodInfo* method, void* obj, void** params, Il2CppException** exc);
```

Semantics that matter to a mod author:

- `obj` is the `this` pointer for instance methods, `NULL` for statics.
- `params` is an array of argument *addresses* (`void**`). For an `int`
  parameter, pass a pointer to an `int`. For a reference type, pass a pointer to
  the pointer. For a value type passed by value, the runtime's
  `il2cpp_runtime_invoke` expects the value's address; the alternative
  `il2cpp_runtime_invoke_convert_args` takes `Il2CppObject**` and converts.
- The return value is `Il2CppObject*`, i.e. **boxed**. An `int` return is a
  pointer to a boxed `System.Int32`, which must be unboxed with
  `il2cpp_object_unbox`. A reference-type return is the object pointer itself.
- A managed exception is not a C++ exception. It is written to `*exc` and the
  function returns `NULL`. A mod that ignores `exc` will misbehave; the SDK's
  example helper checks it and logs the exception class.

`il2cpp_object_unbox` returns a pointer to the value inside the box. The pointer
is valid while the box is alive and not moved. Mocha's helpers copy the value out
immediately; they do not retain the pointer.

## 4. Strings

Two encodings meet here: C strings are UTF-8, IL2CPP strings are UTF-16
(`Il2CppChar` is `uint16_t`).

```c
Il2CppString* il2cpp_string_new(const char* str);          /* UTF-8 in */
Il2CppString* il2cpp_string_new_utf16(const Il2CppChar* text, int32_t len);
Il2CppChar*   il2cpp_string_chars(Il2CppString* str);      /* UTF-16 out */
int32_t       il2cpp_string_length(Il2CppString* str);
```

A C string passed to `il2cpp_string_new` is interpreted as UTF-8 and converted.
A string returned from managed code must be converted back explicitly; the
example helper `invoke_string` calls `string_chars`/`string_length` and encodes
UTF-8. Mods that skip this step and cast `Il2CppString*` to `char*` will read
UTF-16 bytes as if they were UTF-8.

The test app's `Echo(string)` round-trips this path: the mod passes `"mocha"`,
Unity returns `"echo:mocha"`, and `hello.result` records `echo=echo:mocha`.

## 5. Fields and statics

```c
FieldInfo*       il2cpp_class_get_field_from_name(Il2CppClass* klass, const char* name);
void             il2cpp_field_static_get_value(FieldInfo* field, void* value);
void             il2cpp_field_static_set_value(FieldInfo* field, void* value);
void             il2cpp_field_get_value(Il2CppObject* obj, FieldInfo* field, void* value);
void             il2cpp_field_set_value(Il2CppObject* obj, FieldInfo* field, void* value);
size_t           il2cpp_field_get_offset(FieldInfo* field);
```

Static and instance access take an output buffer whose size must match the field
type exactly; the API does not validate it. `il2cpp_field_get_offset` is the
escape hatch for direct memory access when the caller already knows the type and
lifetime rules. Mocha exposes it because some mods need it, but the SDK
documentation warns that raw offsets bypass type checks.

Verified: the test app's static `Counter` reads 0 before the call and 1 after
`Increment()`, recorded in `hello.result` as `counter_before=0` /
`counter_after=1`.

## 6. Threads and the GC

```c
Il2CppThread* il2cpp_thread_attach(Il2CppDomain* domain);
void          il2cpp_thread_detach(Il2CppThread* thread);
void          il2cpp_gc_disable(void);
void          il2cpp_gc_enable(void);
```

Any thread that touches IL2CPP objects must be attached to the domain. The mod
initialization thread already is (it is the `il2cpp_init` caller thread), and
Mocha calls `il2cpp_thread_attach` anyway so the contract is explicit rather than
inherited. A mod that creates its own thread must attach it itself; the SDK does
not do that on the mod's behalf, and the header says so.

`il2cpp_gc_disable`/`il2cpp_gc_enable` bracket a critical section where raw
object pointers must not be invalidated by a collection. They are exposed
because some interop patterns need them; they are not a general-purpose
performance switch, and the test mods do not use them.

## 7. AOT constraints that affect interop

From Unity's "IL2CPP limitations" page (retrieved 2026-11-19):

- Reflection works, but code the compiler cannot see may have been stripped.
  A method that the managed code never calls directly and no `link.xml`/attribute
  preserves may simply not exist in `libil2cpp.so`, and `class_get_method_from_name`
  will return `NULL`.
- `Reflection.Emit` is unavailable, so mods cannot generate types at runtime.
- Generic instances must be known at compile time unless the compiler emitted
  the shared fallback; `MakeGenericType`/`MakeGenericMethod` at runtime are
  restricted.
- Methods marshalled to native function pointers must be static and carry
  `[MonoPInvokeCallback]`.

For a native mod this means: resolve methods defensively, treat a `NULL` result
as "not present in this build", and do not assume that a type visible in the
editor exists in the shipped player. Mocha's `find_method` helper returns
`NULL` and the example mods log a clear failure instead of dereferencing it.

## 8. What Mocha exposes

`MochaIl2CppApi` (`sdk/include/mocha/mocha_api.h`) is an append-only struct of
27 function pointers, grouped as:

| Group | Functions |
|---|---|
| Domain / thread | `domain_get`, `thread_attach`, `thread_detach`, `domain_get_assemblies` |
| Images / classes | `assembly_get_image`, `class_from_name`, `image_get_name`, `class_get_name`, `class_get_namespace` |
| Methods | `class_get_method_from_name`, `method_get_name`, `method_get_param_count`, `runtime_invoke` |
| Strings | `string_new`, `string_chars`, `string_length` |
| Objects | `object_new`, `object_unbox`, `object_get_class` |
| Fields | `class_get_field_from_name`, `field_static_get_value`, `field_static_set_value`, `field_get_value`, `field_set_value`, `field_get_offset` |
| GC | `gc_disable`, `gc_enable` |

The table is filled by the loader after the domain exists and before the first
mod entry point is called, so a mod can use it from inside `mocha_mod_init`. It
stays valid until shutdown. Fields are appended, never reordered; `struct_size`
lets a loader and a mod detect which side is older.

The SDK deliberately does not expose every IL2CPP function. New functions are
added when a real mod needs them, with the ABI-version rules in
`docs/specifications/mod-abi.md`.

## 9. What is not claimed

- Only the functions listed above were exercised. The other ~216 exports were
  observed to exist, not called.
- Overload resolution by argument count is a real limitation; no claim is made
  that arbitrary game APIs are reachable through this surface.
- No claim about generic method invocation from native code. It was not needed
  for the test app and was not attempted.
- Behaviour was verified on Unity 6000.6.9f1 only.
