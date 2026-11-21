# Unity IL2CPP Loading on Android

Research date: 2026-11-19
Scope: how a Unity 6 Android IL2CPP player boots from Java to a live IL2CPP
domain, and where Mocha attaches to that sequence. Statements marked
"verified" were checked against the Unity 6000.6.9f1 test APK built in this
repository and the run recorded in `artifacts/emulator-20261118-191549/`.
Statements marked "background" come from vendor documentation and are not
things Mocha depends on at runtime.

## 1. What IL2CPP is

IL2CPP is Unity's ahead-of-time (AOT) scripting back end. During a build Unity:

1. Compiles C# to managed assemblies with Roslyn.
2. Applies managed code stripping.
3. Converts all managed assemblies to C++.
4. Compiles that C++ with the platform's native compiler.
5. Produces the platform artifact (for Android, `libil2cpp.so` inside the APK).

Source: Unity 6000.0 Manual, "Introduction to IL2CPP"
(<https://docs.unity3d.com/6000.0/Documentation/Manual/il2cpp-introduction.html>,
retrieved 2026-11-19).

The operational consequence is the important one for a mod loader: **an IL2CPP
player ships no managed execution engine**. The C# code has already become
native code. `.dll` files that remain in the build are metadata and resource
carriers used by the IL2CPP runtime for reflection, not assemblies that a CLR
can execute. There is no JIT and no IL interpreter in the player.

This is why mod loaders that support C# on IL2CPP titles ship a separate
managed runtime. UnityDoorstop's own README states the constraint directly:
"Il2Cpp cannot run managed assemblies" and Doorstop therefore hosts CoreCLR
alongside the player. Mocha draws the same conclusion and does not pretend to
load managed assemblies (see ADR 0005).

## 2. The Android boot sequence

The observable sequence for the test app is:

```
Android app process (Zygote fork)
  └─ Java: com.unity3d.player.UnityPlayerGameActivity / UnityPlayer
      └─ System.loadLibrary("main")            → lib/<abi>/libmain.so
          └─ JNI_OnLoad(JavaVM*, void*)        → registers native methods
      └─ NativeLoader.load(String)             → JNI native call
          └─ libmain dlopen("libunity.so")     → libunity.so JNI_OnLoad
          └─ libmain dlopen("libil2cpp.so")    → libil2cpp.so JNI_OnLoad
              └─ il2cpp_init / il2cpp_init_utf16
                  └─ IL2CPP domain created, assemblies registered
                      └─ Unity player continues with managed startup
```

Two details in that path matter:

- `libmain.so` is a small JNI shim. In the Unity 6000.6.9f1 test APK it exports
  `JNI_OnLoad`, imports `dlopen`/`dlsym`/`dlerror` from libc, and loads the two
  larger libraries from the same directory. It does not itself contain the
  runtime.
- `com.unity3d.player.NativeLoader.load(String)` returns a boolean and takes the
  native library directory as an argument. `unload()` is the matching teardown
  entry point. Mocha's shim implements both under the same class and signatures,
  so the Java side is unchanged (see ADR 0001).

This ordering is what makes a proxy `libmain.so` viable: whoever owns
`NativeLoader.load` controls when `libunity.so` and `libil2cpp.so` are mapped,
and therefore controls the window in which the IL2CPP entry points can still be
intercepted.

## 3. `il2cpp_init` and the exported C API

`libil2cpp.so` is not a black box. It exports a C API that the generated code
and embedders use. Verified on the test APK with NDK r27c `llvm-nm`:

| Observation | Value |
|---|---|
| Defined `il2cpp_*` exports in `libil2cpp.so` | 243 |
| `il2cpp_init` | exported |
| `il2cpp_init_utf16` | exported |
| `il2cpp_domain_get_assemblies`, `il2cpp_class_from_name`, `il2cpp_runtime_invoke`, ... | exported |

`il2cpp_init(const char* domain_name)` creates and returns the IL2CPP domain;
`il2cpp_init_utf16(const Il2CppChar*)` is the UTF-16 variant. Which one a
player calls depends on the Unity build. The test player called
`il2cpp_init`; the runtime log records the domain handle it returned:

```
il2cpp_init returned the IL2CPP domain (0x1)
```

and, once the domain existed:

```
19 assemblies visible
Unity version 6000.6.9f1
```

The assembly count is what `il2cpp_domain_get_assemblies` reported for this
build. It is a property of the test app, not a constant of Unity.

The export set is the reason Mocha's SDK is a C ABI: every interop operation is
a `dlsym` lookup on `libil2cpp.so`, so the loader does not link against Unity at
build time and survives symbol-set changes between Unity versions as long as the
functions it names still exist.

## 4. Timing: why the loader cannot run earlier or later

There are only three interesting moments in this boot path:

1. **Before `libil2cpp.so` is mapped.** Nothing useful exists yet. Calling
   `il2cpp_*` here fails.
2. **After `libil2cpp.so` is mapped but before `il2cpp_init` returns.** The
   entry points exist but the domain does not, so domain-dependent calls fail or
   race. `il2cpp_init` itself is still executing on the player thread.
3. **After `il2cpp_init` returns.** The domain exists, assemblies are
   registered, and the C API is usable.

Mocha runs mod initialization at the end of moment 3, synchronously on the
`il2cpp_init` caller thread. The gate's wrapper calls the real `il2cpp_init`,
waits for it to return, then invokes the registered runtime-ready callback:

```
il2cpp_init returned the IL2CPP domain (0x1)
running mod initialization on the il2cpp_init thread
...
mod initialization returned to the engine
```

This placement was not arbitrary. Running mods from a separate worker thread
after the fact produced a hang in a mod that touched `Application.persistentDataPath`:
the Unity player had not finished bringing the managed side up, and the call
blocked. Running on the engine thread, after `il2cpp_init` has returned but
before the player proceeds, gives mods a fully formed domain with no
cross-thread startup race. The trade-off is that slow mods delay startup; that
is documented in ADR 0003.

## 5. What "hooking" means here

Mocha does not write a trampoline into `libil2cpp.so` code. It intercepts the
*lookup* of `il2cpp_init` inside `libunity.so` by rewriting the dynamic
relocation slot that `libunity.so` uses for `dlsym`. That is enough because the
player resolves its entry points with `dlsym` rather than by direct import.

Verified on the test APK: `libunity.so` imports `dlsym@LIBC` and has exactly one
`R_AARCH64_JUMP_SLOT` relocation against it (r_offset `0x141d3c0` in that build;
330 `JUMP_SLOT` and 6 `GLOB_DAT` relocations in total, with `DT_JMPREL` /
`DT_RELA` tables present). Because the slot is unique in this build, patching it
cannot affect an unrelated call. The gate still verifies the relocation symbol
name before writing, rather than assuming an offset.

The mechanism is described in `docs/research/android-native-loading.md` §4–5 and
in ADR 0002. Its relevance here is architectural: Mocha depends on the *symbol
resolution* behaviour of `libunity.so`, not on instruction patterns or on a
particular Unity build hash. A Unity build that resolved `il2cpp_init` through a
direct PLT import instead of `dlsym` would not be intercepted by this gate; that
is a stated limitation, not a hidden assumption.

## 6. Why mods are native, not C#

Given an AOT-only player, there are two honest options for a mod:

- Ship a managed runtime (CoreCLR/Mono) inside the loader and bridge it to
  IL2CPP, as UnityDoorstop + Il2CppInterop and BepInEx do on desktop. That is a
  large, separate subsystem: it requires generating interop assemblies from
  IL2CPP metadata (Cpp2IL/Il2CppInterop) and keeping a second GC and thread
  model coherent with Unity's.
- Define a stable native C ABI and let mods call the IL2CPP C API directly.

Mocha implements the second and documents the first as future work. This is why
`MochaIl2CppApi` in `sdk/include/mocha/mocha_api.h` is a table of function
pointers resolved at runtime: a mod compiled against the SDK has no link-time
dependency on Unity, and the same binary works across players that export the
same functions.

The API table is append-only and carries `struct_size` and `MOCHA_ABI_VERSION`
so a mod built against an older SDK keeps working with a newer loader (and a mod
that requires newer fields can detect an older loader and fail cleanly).

## 7. What was actually verified

| Claim | Evidence |
|---|---|
| Real Unity IL2CPP player booted | `runtime log`: `Unity version 6000.6.9f1`; 19 assemblies |
| `il2cpp_init` was intercepted | log: `intercepted libunity il2cpp_init lookup` |
| Domain was created and usable | log: `il2cpp_init returned the IL2CPP domain (0x1)` |
| Mods ran after domain creation, on engine thread | log: `running mod initialization on the il2cpp_init thread`, then mod `loaded` lines |
| Managed calls work through the C API | `hello.result`: `add=2+3=5`, `echo=echo:mocha`, `counter_after=1` |
| `libil2cpp.so` export surface | `llvm-nm -D`: 243 defined `il2cpp_*` symbols |
| `libunity.so` resolves `dlsym` through one JUMP_SLOT | `llvm-readelf -r`: one `R_AARCH64_JUMP_SLOT` at `0x141d3c0` |

Artifacts: `artifacts/emulator-20261118-191549/{loader-summary.txt,mocha.log,hello.result,package.txt}`.
Environment: x86_64 API 37 emulator with the Android native bridge, executing
the `arm64-v8a` player through Berberis — see
`docs/testing/emulator-notes.md`. This is a real Android runtime and a real
IL2CPP player, but not ARM64 hardware.

## 8. What is not claimed

- No claim about Unity versions other than 6000.6.9f1. The gate matches symbol
  names and relocation structure, so it is *expected* to work across Unity 6
  builds, but only one version was executed.
- No claim about Mono players. A Mono build has a managed runtime and a
  different boot path; Mocha's gate would not find an `il2cpp_init` lookup.
- No claim about managed assembly mods. They are out of scope for this
  implementation.
- No claim about Quest at the research date. Update (2026-11-20): the boot
  sequence described here was observed on a physical Meta Quest 3 (Android 14,
  4 KiB pages, non-root) running the test player, including the gate, the
  domain, the 19 assemblies and `InitializeScriptEngine OK`; see
  `docs/testing/results-quest-2026-11-20.md`. The Quest runtime still adds
  VrApi/OpenXR and an OS permission model that this test app does not exercise,
  so an immersive Quest title remains unverified.
