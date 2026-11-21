# Repository and Environment Assessment

Date: 2026-11-18
Scope: Phase 1 inspection of the Mocha workspace and the available development environment.

Status update (2026-11-20): this document is the Phase 1 snapshot. At that
point no Quest was attached, so L4 was recorded as unavailable below. A
physical Meta Quest 3 was later connected and L4 passed for the test player
(Android 14, 4 KiB pages, non-root); see
`docs/testing/results-quest-2026-11-20.md`. The inspection findings in §1–§2
are preserved as written; the availability statements in §2.1 and the L4 row
in §3 describe the Phase 1 environment, not the current one.

## 1. Repository state

`/home/cloudy/Documents/mocha` was empty at the start of this work:

- No `.git` directory, no source tree, no build files, no tests, no documentation,
  no previous experiments, and no `AGENTS.md` / `CLAUDE.md` repository instructions.
- No Mocha source exists anywhere under the user's home directory (only unrelated
  matches such as npm's `mocha` test framework cache).

Therefore:

| Requested inspection item | Result |
|---|---|
| Languages / frameworks / build systems / dependencies | None present |
| Current architecture / functionality | None present |
| Incomplete or obsolete implementations | None present |
| Build scripts and tooling | None present |
| Existing tests | None present |
| Existing documentation | None present |
| Abandoned experiments | None present |

**Conclusion:** there is nothing to preserve, and no legacy implementation constrains
the design. The rewrite is a greenfield implementation. The "do not delete before
assessment" rule is satisfied trivially. The repository is initialized from scratch
during this project.

## 2. Development environment (verified by direct inspection)

| Tool | Finding | Evidence |
|---|---|---|
| OS / CPU | Linux x86_64, KVM available | `uname -m`, `/dev/kvm` present (rw) |
| Unity Editor | 6000.6.9f1, Android build support installed (AndroidPlayer, LinuxStandalone) | `~/Unity/Hub/Editor/6000.6.9f1` |
| Unity licensing | Unity Personal entitlement valid through 2026-12; headless entitlement present; batchmode works | `~/.config/unity3d/Unity/licenses/UnityEntitlementLicense.xml`; successful `-createProject` |
| Unity Android NDK | r27c (27.2.12479018) bundled | `.../AndroidPlayer/NDK/source.properties` |
| Unity Android SDK | bundled SDK, build-tools + platforms + platform-tools | `.../AndroidPlayer/SDK` |
| Unity OpenJDK | bundled | `.../AndroidPlayer/OpenJDK` |
| Standalone Android SDK | `~/Android/Sdk`, build-tools 36.0.0, platform android-37.0 (API 37), platform-tools, emulator | directory listing |
| AVD | `Resizable_Experimental`, x86_64, API 37.2 Google APIs, 16 KB page size, 8 GB RAM, 4 cores | `~/.android/avd/.../config.ini` |
| Emulator | Boots headless in ~38 s; KVM accelerated | emulator log |
| Waydroid | installed, currently inactive, x86_64 image (LineageOS-based), no ARM translation assumed | `waydroid_base.prop` |
| ADB | installed; **no physical device attached** | `adb devices -l` |
| .NET SDK | 8.0.100, 9.0.123, 10.0.114 | `dotnet --list-sdks` |
| Native host toolchain | clang, clang++, CMake, Ninja, make, git | `command -v` |
| Python | python3 + uv | `command -v` |
| Network | outbound HTTPS available (GitHub reachable) | `curl -I https://github.com` |

### 2.1 Not available

- **No physical Meta Quest was connected at the time of this inspection.**
  Quest hardware validation could not be claimed then. (A Quest 3 was attached
  later, on 2026-11-20, and L4 was run; see the status update above.)
- **No Gradle CLI** on `PATH`; Unity uses its bundled Gradle, and the Android SDK
  build-tools (`apksigner`, `zipalign`) are available.
- **No Mono / mcs**. .NET is available but Android IL2CPP does not include a
  managed runtime, so C# mod hosting remains a separate technical problem.
- **No physical ARM64 Android device at the time of this inspection.** The
  emulator and Waydroid are x86_64 hosts. (A physical Meta Quest 3, arm64-v8a,
  was attached later on 2026-11-20.) However, the AVD used for this project (`Resizable_Experimental`,
  API 37.2, 16 KB pages) has the Android native bridge enabled
  (`ro.dalvik.vm.native.bridge=libndk_translation.so`,
  `ro.enable.native.bridge.exec=1`, `ro.product.cpu.abilist=x86_64,arm64-v8a`)
  and runs the arm64-v8a-only Unity APK under Berberis 16.0.0. This exercises
  real ARM64 guest code but is **translation, not an ARM64 kernel or an ARM64
  CPU**; it is not a substitute for L4 hardware validation. See
  `docs/testing/emulator-notes.md` for the exact properties and caveats.

## 3. Environment consequences for the test strategy

The three-level test hierarchy from the project brief maps onto this environment as:

| Level | Environment available | What can actually be claimed |
|---|---|---|
| L1 desktop/unit | Host clang, Python, .NET | Manifest validation, version/dependency logic, ELF/APK patcher logic, unit tests |
| L2 Android runtime | x86_64 API 37 emulator running arm64-v8a code through the native bridge (and optionally Waydroid) | APK install/launch, native library loading, bootstrap logs, filesystem/JNI behaviour, ARM64 instruction execution under translation |
| L3 Unity IL2CPP | Unity 6000.6.9f1 IL2CPP test app (arm64-v8a) on the x86_64 emulator | IL2CPP API resolution, class/method invocation, mod discovery and init, failure isolation |
| L4 Quest | **unavailable at the Phase 1 inspection** (later: Quest 3, Android 14, PASS for the test player) | Nothing could be claimed as Quest-verified at the time; see `docs/testing/results-quest-2026-11-20.md` |

A controlled Unity IL2CPP Android test application is therefore built in this
repository and used as the first compatibility target. Unity 6 removed the
`x86_64` Android player architecture, so the app is built **only for
`arm64-v8a`** and executed on the x86_64 emulator through
`libndk_translation`/Berberis. This is the configuration that was actually run;
the earlier plan to ship a separate x86_64 build was not possible.

## 4. Recommended strategy (initial, subject to research evidence)

1. Build a native C++ loader (`libmocha.so`) with a deterministic bootstrap and
   no link-time dependency on Unity libraries.
2. Inject it into a real Unity IL2CPP APK through a reproducible, offline APK
   patcher; do not rely on runtime tricks that require root or a custom ROM.
3. Integrate with IL2CPP through its exported C API resolved at runtime.
4. Ship native mods first with a stable C ABI; treat managed C# mod hosting as a
   separately researched capability rather than faking an assembly loader.
5. Verify every layer on the emulator; at the time of this inspection, mark
   ARM64/Quest results unverified until hardware was available. (Completed on
   2026-11-20 with a physical Quest 3 for the test player; broader titles and
   devices remain untested.)

Each point is validated or revised by the research documents in `docs/research/`
and the ADRs in `docs/decisions/`.
