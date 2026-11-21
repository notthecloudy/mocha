# Android Native Loading and ARM64

Research date: 2026-11-19
Scope: the Android/ARM64 mechanics Mocha depends on. Every mechanism listed here is
either exercised by Mocha's tests or explicitly marked as background.

## 1. Native libraries inside an APK

An Android APK is a ZIP. Native shared objects are stored under `lib/<abi>/`, for
example:

```
lib/arm64-v8a/libmain.so
lib/arm64-v8a/libunity.so
lib/arm64-v8a/libil2cpp.so
```

The package manager extracts or maps these according to `android:extractNativeLibs`
/ `useLegacyPackaging`. When libraries are stored uncompressed and page-aligned in
the APK, the loader maps them directly; this is why `zipalign` alignment matters
(below).

The ABI directory is selected from `Build.SUPPORTED_ABIS`. An APK containing only
`lib/arm64-v8a/` installs on a device whose `ro.product.cpu.abilist` contains
`arm64-v8a`, including through the native bridge described in
`docs/testing/emulator-notes.md`.

## 2. `dlopen`, `dlsym`, `JNI_OnLoad`

The NDK exposes the POSIX dynamic loader (`<dlfcn.h>`):

- `dlopen(path, flags)` maps a library. Flags used by Mocha:
  - `RTLD_NOW` — resolve all symbols immediately (used for `libmocha.so`, so a
    missing symbol fails at load instead of at first call).
  - `RTLD_LAZY` — resolve function symbols on first use (used for Unity's own
    libraries, matching stock `libmain` behaviour).
  - `RTLD_GLOBAL` — make the library's symbols available to subsequently loaded
    libraries. Mocha loads itself `RTLD_GLOBAL` so mods and the gate can resolve
    any Mocha export.
  - `RTLD_LOCAL` is the default for libraries loaded without `RTLD_GLOBAL`.
- `dlsym(handle, name)` resolves a symbol. `dlsym(RTLD_DEFAULT, name)` searches the
  global scope.
- `dladdr(addr, &info)` maps an address back to its shared object; the shim uses it
  to locate `libmocha.so` next to itself when the bare soname lookup fails.
- `dlerror()` returns the last loader error text.

When the JVM loads a library through `System.loadLibrary` or the Unity
`NativeLoader`, it calls that library's `JNI_OnLoad(JavaVM*, void*)`, whose return
value is the JNI version it requires. Android's `JNI_OnLoad` contract is documented
in the Android JNI tips and the JNI specification. Mocha's shim returns
`JNI_VERSION_1_6`, the same version Unity's libraries use.

## 3. Linker namespaces

Since Android 7.0, the dynamic linker isolates applications into linker namespaces.
A process can only `dlopen` libraries that are reachable through its namespace's
search paths and permitted paths; arbitrary paths are rejected even when the file
exists and is readable.

This is directly relevant to mod loading. A mod `.so` placed in a world-readable
location such as `/sdcard/...` may be copyable but not `dlopen`-able, because the
app's namespace does not permit that path. Mocha therefore:

1. Discovers manifests in the app's files directory.
2. Copies the sibling `.so` into the app's own internal native directory
   (`<files>/mocha/native`), which is inside the namespace's permitted paths.
3. `dlopen`s the internal copy.

The namespace observed on the test AVD was `clns-9`, with permitted paths including
`/data`, `/mnt/expand` and `/data/data/dev.mocha.testapp`. That is consistent with
the design above, and the internal-copy path is visible in the verified log
(`mod ... loaded from internal copy`).

Primary source: Android platform documentation, "Linker Namespaces"
(<https://source.android.com/docs/core/architecture/vndk/linker-namespace>,
retrieved 2026-11-19).

## 4. The ELF dynamic section

Mocha's runtime gate does not detour machine code. It rewrites the *relocation*
that `libunity.so` uses for `dlsym`. That requires walking the ELF dynamic table:

| Structure | Meaning |
|---|---|
| `PT_DYNAMIC` (`Elf64_Phdr`) | Locates `Elf64_Dyn[]` |
| `DT_SYMTAB` / `DT_STRTAB` | Symbol and string tables |
| `DT_JMPREL` + `DT_PLTRELSZ` | PLT relocations (the `dlsym` jump slot) |
| `DT_RELA` + `DT_RELASZ` | RELA relocations (AArch64 uses RELA) |
| `DT_REL` + `DT_RELSZ` | REL relocations (not used on AArch64, handled for safety) |

On AArch64 the interesting relocation types are:

| Type | Value | Use |
|---|---|---|
| `R_AARCH64_JUMP_SLOT` | 1026 | PLT/GOT entry for an imported function |
| `R_AARCH64_GLOB_DAT` | 1025 | GOT entry for an imported data/function symbol |

Both are patched by writing the replacement address into the 8-byte slot identified
by `r_offset`. The gate accepts either type so it works whether the library was
linked with normal PLT slots or with `-fno-plt`-style GOT entries.

Reference: ARM IHI 0056 ("ELF for the Arm 64-bit Architecture"), relocation
numbers for AArch64.

## 5. Writing to a relocated slot

The slot lives in a `PT_LOAD` segment. Its permissions are read from
`/proc/self/maps`: Mocha finds the mapping containing the slot address and takes the
protections from it. The gate then:

1. `mprotect`s the containing page(s) to `PROT_READ | PROT_WRITE` (preserving
   execute where it was set).
2. Writes the 8-byte address.
3. Restores the original protections.
4. Calls `__builtin___clear_cache` over the written range.

On ARM, instruction and data caches are not guaranteed coherent. Even though the
relocation slot is data, the compiler intrinsic is used so the write is visible to
any subsequent instruction fetch through that GOT/PLT entry. This is the
`mprotect` + clear-cache pattern used by every in-process patcher on Android.

Verified behaviour on the test app:

```
runtime gate installed on libunity.so (dlsym slot, base=0x7a6d55004000)
intercepted libunity il2cpp_init lookup
intercepted libunity il2cpp_init_utf16 lookup
```

The base address is per-run and not stable.

## 6. 16 KiB page sizes

Android 15 introduced 16 KiB page-size devices, and Android 16 requires
compatibility for apps targeting it. An ELF that assumes 4 KiB pages can fail to
load, and an APK whose uncompressed libraries are aligned to 4 KiB can be rejected
or mis-mapped.

Mocha therefore builds both `libmocha.so` and the `libmain.so` shim with:

```
-Wl,-z,max-page-size=16384
-Wl,-z,common-page-size=16384
```

This sets the maximum page size recorded in the `PT_LOAD` alignment. The test APK is
produced with `zipalign -P 16` so the stored libraries are 16 KiB aligned inside the
ZIP. The test AVD uses 16 KiB pages, so a regression here fails the run rather than
passing silently on a 4 KiB device.

`-Wl,-z,now` and `-Wl,-z,relro` are also set on both libraries so imported symbols
are resolved eagerly and the relocation tables are made read-only after load.

Primary source: Android developer documentation, "Support 16 KB page sizes"
(<https://developer.android.com/guide/practices/page-sizes>, retrieved 2026-11-19).

## 7. Threading and JNI attachment

`JNI_OnLoad` runs on a thread already attached to the JVM. `NativeLoader.load` is
called from the Unity player thread, also attached. The mod initialization path runs
synchronously on the `il2cpp_init` caller thread, which is already attached to both
JNI and IL2CPP. Mocha still calls `il2cpp_thread_attach(domain)` before invoking
mods so that a mod which stores an IL2CPP object and later touches it from the same
thread has a valid IL2CPP thread context. Background JNI work, if a mod starts it,
must attach with `AttachCurrentThread`; the SDK does not do this on a mod's behalf.

## 8. What is not claimed

- No claim is made about OEM linker namespace differences beyond the tested
  configurations.
- At the research date no 4 KiB device had been tested, because the emulator
  image was 16 KiB. Update (2026-11-20): a physical Meta Quest 3 reports 4 KiB
  pages and ran the 16 KiB-aligned build; see
  `docs/testing/results-quest-2026-11-20.md`. Other 4 KiB devices remain
  untested.
- Quest's own loader behaviour was unverified at the research date. Update
  (2026-11-20): on Quest 3 (Android 14) the linker namespace `clns-6` denied
  direct `dlopen` of external mod libraries and the internal-staging fallback
  succeeded, as recorded in `docs/testing/results-quest-2026-11-20.md`. Other
  Quest models and OS releases remain untested.
