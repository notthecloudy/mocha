# Known Limitations

This is the authoritative list of what Mocha does not do or has not verified.
It is deliberately blunt; the engineering report and README defer to it.

## 1. Verification gaps

- **Quest verification is narrow.** L4 passed on a Meta Quest 3 running
  Android 14/API 34, build `UP1A.231005.007.A1`, on 4 KiB pages, as a stock
  `user` build with no root. Quest 2, Quest Pro, newer Quest OS releases,
  16 KiB-page Quest devices, and commercial titles were not tested. The test
  player ran as a panel app, not an immersive VR title, so VR runtime
  initialization, controller input, and frame-loop interaction were not
  exercised.
- **One physical ARM64 device.** The Quest 3 is physical ARM64 and is the only
  one. The L2/L3 emulator runs used an x86_64 API 37 image with Berberis
  (`libndk_translation.so`) translating the arm64-v8a player; timing, cache
  behavior, atomics, and crash symbolization differ from the device.
- **One Unity version.** 6000.6.9f1 only. The gate's ELF relocation handling
  and the set of exported `il2cpp_*` functions are generic in design but were
  not exercised on another Unity release.
- **One game shape.** The test app is a minimal single-scene player. No real
  commercial title, split APK, asset bundle, or anti-tamper configuration was
  tried.
- **Shutdown/unload not verified.** The code path exists, but the captured runs
  force-stop the app; `JNI_OnUnload`/`mocha_shutdown` behavior is not a test
  result.
- **No long-running, suspend/resume, or lifecycle testing.** Only cold launch
  through `InitializeScriptEngine OK`.

## 2. Mod model

- **Mods are native C/C++ shared libraries.** There is no CLR in an IL2CPP
  player and Mocha does not embed one. C# mods, BepInEx plugins, Harmony
  patches, IL weaving, and Reflection.Emit are not supported. A mod is
  `lib<name>.so` with a C entry point (ADR 0005).
- **IL2CPP access is a 27-function C table**, with 7 required symbols. It is
  not a complete binding of the 243 exported functions, and it exposes no
  managed type construction, generics, GC handles, or exception objects.
- **Lookup is by namespace/class/method name and parameter count.** Overloads
  with the same name and arity, generic methods, and stripped methods may be
  unreachable. A mod must handle `NULL` handles.
- **No managed assembly loading.** The `.dll` files in an IL2CPP build are
  metadata, not executable; Mocha does not pretend otherwise.
- **ABI 1 is fixed at compile time.** A mod built for another `MOCHA_ABI_VERSION`
  is rejected before `dlopen`; structs are append-only, so removals are not
  compatible.

## 3. Isolation and safety

- **No sandbox.** A mod is native code in the game process. A crash, `abort()`,
  infinite loop, or memory corruption takes down the player. The loader can
  isolate a mod that returns non-zero or fails to load; it cannot survive a
  native fault.
- **No signature or integrity verification.** Mods are loaded from a
  user-writable external directory and staged to internal storage. Anyone who
  can write there (adb, root, a compromised device, or the user) can cause code
  to run with the app's privileges.
- **Code execution from shared storage is the design.** ADR 0004 explains why;
  the mods directory must be treated as trusted input.
- **Stem collision.** Internal staging uses the library's file stem. Two mods
  with the same `.so` name overwrite each other's staged copy; behavior for
  that pair is undefined. Mod ids are unique, file names are not constrained.
- **No cleanup.** Staged copies and stale `.so` files remain after a mod is
  removed or updated; they are only loaded if a manifest still points at that
  stem.
- **Concurrent file changes** during staging can produce a truncated library;
  the loader reports `failed:dlopen` rather than retrying or locking.

## 4. Startup and lifecycle

- **All mods run synchronously during `il2cpp_init`**, on the engine's init
  thread, before `il2cpp_init` returns (ADR 0003). A slow or blocking mod
  delays game startup; there is no watchdog or per-mod timeout.
- **Early engine state.** The domain and assemblies exist, but scenes, player
  objects, and much of Unity are not initialized. A mod that assumes gameplay
  state will fail; the loader cannot detect this.
- **No later lifecycle hook.** There is no `on_scene_loaded`,
  `on_update`, or shutdown callback from the loader. A mod that needs a later
  point must create and attach its own thread and poll, or install its own
  engine-specific hook (outside Mocha's guarantees).
- **Once-only callback.** If a player initializes IL2CPP more than once or with
  a different domain, mods are not re-run.
- **Mod order is dependency-first and deterministic**, but it is not a
  scheduling guarantee for mod-created threads.

## 5. Platform and packaging

- **ARM64 only in practice.** The build targets `arm64-v8a`; no armeabi-v7a,
  x86, or x86_64 loader is built. Android API 26 is the minimum.
- **The APK is modified.** `libmain.so` is replaced and `libmocha.so` added;
  every native library is re-stored, aligned, and the APK re-signed. This
  changes the signature and invalidates the original signer. Anti-tamper,
  Play Integrity, and store-update flows will notice or refuse it.
- **No installer or patcher UI.** Patching is a Python CLI; mod installation
  is copying files (the test harness does it over adb).
- **16 KiB alignment is built in**, but stock Unity libraries keep whatever
  alignment the title shipped. Mocha cannot fix a game whose own libraries are
  4 KiB-aligned and therefore unloadable on a 16 KiB device. The 16 KiB-aligned
  build was also loaded and executed on the Quest's 4 KiB pages; this shows it
  is accepted there, not that 16 KiB page behavior was tested on Quest.
- **Storage selection is automatic.** External app files are preferred when
  writable, otherwise internal. There is no user setting to force one.
- **Deployment on a non-root user build is unresolved.** On Quest 3, mod files
  pushed into external app storage by adb as `shell` had to be made
  world-readable (`0666`) before the app could open them; `run-as` cannot write
  `/sdcard`. Internal staging through `run-as` works because the test APK is
  debuggable. A release build needs an app-UID installer or another
  user-facing delivery path, which does not exist yet.
- **Fixed logging.** `mocha.log` is appended and rotated at 1 MiB to
  `mocha.log.1`; there is no level configuration, no alternate path, and no
  log shipping.
- **No loader configuration file.** Discovery paths, the readiness timeout,
  and log behavior are compile-time decisions; there is no `mocha.json` or
  settings UI. Mods read their own configuration (the manifest has no settings
  block).

## 6. Dependency handling

- Manifests declare dependencies with semver constraints; planning is
  deterministic, dependency-first, and rejects missing dependencies, cycles,
  version mismatches, and duplicate ids. The unit tests cover these cases.
- **No repository, download, or update mechanism.** Everything a mod needs
  must already be present locally.
- **No conflict solver across alternative versions.** Each mod id appears once;
  the loader checks declared constraints against the discovered manifest
  versions rather than choosing among multiple installed versions.
- Optional dependencies are supported; the absent case is covered by tests.

## 7. Scope boundaries

- **No game-specific code.** Mocha has no hardcoded game ids, assemblies,
  hooks, or APIs, and no H3VR2 integration in core. Any future game
  integration is separate and would use the public mod ABI.
- **No multiplayer mod** was designed, implemented, or researched.
- **No anti-cheat bypass, DRM circumvention, or detection evasion.** The
  loader is not designed for those uses and does not implement them.

## 8. Licensing

There is no top-level `LICENSE` file yet. Before distributing source or
binaries, choose a licence and satisfy the nlohmann/json MIT notice and the
NDK/libc++ Apache-2.0 notice (`docs/dependencies.md` §5). The Unity test APK is
not redistributable without checking Unity's terms.
