# Dependency and Licence Inventory

Date: 2026-11-19. This covers what is compiled into or required to build the
repository as it stands. It is an inventory, not legal advice.

## 1. Compiled into the loader

| Component | Version | Location | Licence | How used |
|---|---|---|---|---|
| nlohmann/json | 3.11.3 | `native/third_party/nlohmann/json.hpp` | MIT (`LICENSE.MIT` alongside) | header-only JSON parsing for manifests |
| libc++ (static) | NDK r27c (clang 18.0.3) | NDK sysroot, linked statically | Apache-2.0 with LLVM exception | C++ standard library |
| Android platform libs | device API 26+ | `liblog.so`, `libdl.so`, `libm.so`, `libc.so` | part of Android/`bionic` | logging, `dlopen`/`dlsym`, math, libc |

The loader links `libc++` statically, so `libmocha.so` has no
`libc++_shared.so` dependency (verified: `NEEDED` is `liblog.so`, `libdl.so`,
`libm.so`, `libc.so`). The shim is likewise statically linked; its export table
contains libc++ symbols as a side effect, but only `JNI_OnLoad`/`JNI_OnUnload`
are its contract.

Mocha contains **no** third-party hooking, injection, ELF-patching, or
managed-runtime code. The gate, shim, manifest/plan/semver code, and patcher
are original code in this repository.

## 2. Build and packaging tools (not redistributed)

| Tool | Version used | Licence | Role |
|---|---|---|---|
| Android NDK | 27.2.12479018 (r27c) | Apache-2.0 (with LLVM/libc++ notices) | cross-compilation |
| clang / lld | 18.0.3 (r522817c) | Apache-2.0 with LLVM exception | compiler/linker |
| CMake | ≥ 3.22 | BSD-3-Clause | build system |
| Ninja | system | Apache-2.0 | build generator |
| Python | 3.10+ | PSF-2.0 | patcher runtime (stdlib only) |
| Android build-tools `zipalign` | 35/36 | Apache-2.0 | archive alignment |
| Android build-tools `apksigner` | 35/36 | Apache-2.0 | v2/v3 signing and verification |
| adb / emulator | platform-tools | Apache-2.0 | deployment and testing |

The patcher invokes `zipalign` and `apksigner` as external programs; it does
not vendor them.

## 3. Test-only application

`testapp/` is a Unity project. Its runtime and editor are **not** part of the
loader and are not redistributed by the loader build.

| Item | Version | Licence |
|---|---|---|
| Unity Editor / Android player | 6000.6.9f1 | Unity Personal/Pro terms; not open source |
| `com.unity.ide.rider` | 3.1.0 | Unity package terms |
| `com.unity.ide.visualstudio` | 2.0.26 | Unity package terms |
| `com.unity.toolchain.linux-x86_64-linux` | 1.1.1 | Unity package terms |
| Unity modules (`com.unity.modules.*`) | 1.0.0 | Unity package terms |

The test APK contains Unity's engine, IL2CPP runtime, and the managed test
assemblies. It is a local test artifact, not something this project licenses
to anyone. Do not redistribute `testapp/Build/mocha-testapp.apk` without
checking Unity's terms.

## 4. Prior art reviewed, not linked

These informed the design and are named in the research documents. No code
from them is included.

| Project | Licence (as published) | Relationship |
|---|---|---|
| UnityDoorstop | LGPL-2.1 | prior art for proxying `libmain.so` / early injection; **not** linked |
| BepInEx | LGPL-2.1 | prior art for IL2CPP mod loading on PC; **not** linked |
| Il2CppInterop | LGPL-3.0 | prior art for generated interop; **not** used |
| MelonLoader | Apache-2.0 | prior art; Android/Quest support noted as WIP; **not** linked |

Because Mocha does not link or copy LGPL code, the loader itself is not
derived from those projects. The design decisions are documented in
`docs/decisions/0001`–`0006` and the research files, with sources and dates.

## 5. Project licensing status

There is currently **no top-level `LICENSE` file** in the repository. That is
a gap to resolve before publishing or distributing binaries, not an implicit
grant. The `testapp` project and the third-party header retain their own terms
as described above.

If the loader is released under a permissive licence, the practical
obligations are:

- retain `native/third_party/nlohmann/LICENSE.MIT` and the nlohmann copyright
  notice with source distributions;
- include the LLVM/libc++ Apache-2.0 notice for the statically linked runtime
  in binary distributions, as the NDK's `NOTICE` file describes;
- do not describe the project as LGPL/GPL-derived from the prior-art projects
  unless code is actually taken from them.

## 6. Runtime dependencies of a built loader

`libmocha.so` depends only on platform libraries present on any Android API 26+
device:

```
liblog.so  libdl.so  libm.so  libc.so
```

It does not depend on `libc++_shared.so`, Unity libraries, or any mod. It
resolves `il2cpp_*` symbols at runtime with `dlsym` from the already-loaded
`libil2cpp.so`; those are not link-time dependencies and not redistributed.

The shim (`libmain.so`) has the same platform-only dependency set and is
loaded by Android before the app's Java code runs.

## 7. Mod SDK

`sdk/include/mocha/mocha_api.h` is the only file a mod needs. It is original
code and has no external includes beyond `<stddef.h>`/`<stdint.h>`. Example
mods are built against it with the NDK and link only platform libs.

## 8. Summary

- One vendored library: nlohmann/json 3.11.3, MIT, header-only.
- Static libc++ from the NDK: Apache-2.0 with LLVM exception.
- Everything else is either an external build/test tool or the Unity test app.
- No GPL/LGPL code is linked into the loader.
- A repository-level licence and a binary NOTICE are still outstanding.
