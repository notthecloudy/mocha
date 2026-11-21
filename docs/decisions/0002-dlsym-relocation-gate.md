# ADR 0002: Gate `il2cpp_init` by patching `libunity.so`'s `dlsym` relocation

- Status: accepted, implemented
- Date: 2026-11-19
- Code: `native/src/loader/runtime_gate.cpp`
- Related: ADR 0001, ADR 0003, `docs/architecture/boot-sequence.md`

## Context

Mocha must know when IL2CPP is initialized: `il2cpp_domain_get` is only valid
after `il2cpp_init` has created the domain. The loader also needs to run mods
on the thread that performed initialization (ADR 0003).

`libunity.so` does not call `il2cpp_init` through a PLT relocation to an
exported symbol of `libunity.so`; it resolves it dynamically:

```
void* h = dlopen("libil2cpp.so", ...);
void* p = dlsym(h, "il2cpp_init");
```

The exact timing and whether it calls `il2cpp_init` or `il2cpp_init_utf16`
depend on the player. There is no exported hook point, no stable C++
symbol to patch, and modifying `libil2cpp.so` (a large, AOT-generated
library) on disk is undesirable.

Requirements:

- Observe the call without recompiling or patching either Unity library's
  code on disk.
- Do not change behavior for any code other than `libunity.so`'s lookups.
- Fail closed: if the observation cannot be installed, run no mods.
- No dependence on a specific Unity build beyond the standard ELF dynamic
  layout.

## Decision

Interpose `dlsym` **only for `libunity.so`** by replacing the pointer in the
`libunity.so` relocation slot(s) that reference the symbol `dlsym`. The
replacement (`mocha_dlsym`) forwards everything to the real function and wraps
exactly two results:

- `il2cpp_init` → `mocha_il2cpp_init`
- `il2cpp_init_utf16` → `mocha_il2cpp_init_utf16`

Installation steps (`install_runtime_gate`):

1. Locate `libunity.so` with `dl_iterate_phdr`, recording load bias, `PT_PHDR`
   and `PT_DYNAMIC`.
2. Walk `PT_DYNAMIC` for `DT_SYMTAB`, `DT_STRTAB`, `DT_STRSZ`, `DT_JMPREL`,
   `DT_PLTRELSZ`, `DT_PLTREL`, `DT_RELA(SZ)`, `DT_REL(SZ)`.
3. Scan the PLT relocation table (`DT_JMPREL`) first, then `.rela.dyn`/`.rel.dyn`
   as a fallback, for relocations whose type is `R_AARCH64_JUMP_SLOT` (1026)
   or `R_AARCH64_GLOB_DAT` (1025) — with analogous constants for x86_64, arm
   and i386 — and whose symbol name is `dlsym`.
4. For each, replace the 8-byte slot with `&mocha_dlsym`. The page protection
   is read from `/proc/self/maps` for the slot address, `mprotect` adds
   `PROT_WRITE`, the pointer is stored, `__builtin___clear_cache` is issued, and
   the original protection is restored.
5. If at least one slot was replaced and the real `dlsym` could be recovered,
   mark the gate installed. Otherwise report an error and do not install.

Because the shim reports `libunity.so` **before** invoking its `JNI_OnLoad`
(ADR 0001), the slot is already replaced when the engine performs its lookup.
`libil2cpp.so`'s own `dlsym` calls are untouched, so mod lookups and engine
lookups other than these two symbols behave normally.

## Consequences

Positive:

- No on-disk modification of Unity code; the change is a single pointer write
  per matching relocation.
- The interception is narrow: only `libunity.so`'s `dlsym`, only two symbols.
  Everything else logs and forwards.
- Works whether the engine uses `il2cpp_init` or `il2cpp_init_utf16`, and
  whether relocation is eager (`GLOB_DAT`) or lazy (`JUMP_SLOT`).
- The wrapper has the real function pointer, runs it, and only then marks the
  runtime ready; no polling is required to detect init.

Negative and risks:

- Depends on an ELF layout detail: a `dlsym` relocation in `libunity.so`. All
  Unity players observed do this, but a future player could call a cached
  pointer, call through a different mechanism, or be built with `-Bsymbolic`
  paths. In that case installation fails cleanly and mods do not run; it does
  not crash the game.
- Depends on mapping permissions. The code reads `/proc/self/maps` to preserve
  the slot's protection. On a hardened device where `mprotect` on an `r-x`
  page is denied (SELinux/W^X), installation fails closed. Not verified on
  Quest hardware at the time of this ADR; the later Quest 3 run did install the
  gate (see Update below), but other hardened devices remain untested.
- `mprotect` is process-wide for the page; the window is two syscalls and one
  store, done before the engine's own lookups, and no loader code runs
  concurrently at that point.
- The pointer slot may be in a read-only-after-relocation segment; reading the
  original value before writing recovers the real `dlsym` for forwarding, with
  a `RTLD_DEFAULT` lookup as backup.
- Only calls that actually go through `libunity.so`'s relocation are seen. A
  direct global lookup performed by another library would not be.

## Alternatives considered

- **Patch `libil2cpp.so`'s exported `il2cpp_init`** (e.g., inline hook or
  symbol table edit): larger surface, modifies AOT code, and still needs a way
  to run before Unity initializes. Rejected.
- **PLT hooking library (plthook, Dobby)**: solves the same problem with an
  extra dependency and a broader interception model. The relocation walk here
  is small and auditable, and only targets `dlsym`; no third-party runtime is
  linked into the loader. Rejected for size/dependency reasons.
- **Hook `dlsym` globally** (e.g., via exported symbol interposition): affects
  every consumer, including mods that expect real `dlsym` semantics, and relies
  on symbol resolution order. Rejected.
- **Poll from a thread**: `il2cpp_domain_get` is not safe to call before init;
  polling cannot distinguish "not yet" from "invalid". Rejected as a hook
  mechanism; the timeout wait remains only as a fallback after arming.
- **Do nothing and load mods after the first frame**: no stable hook point
  without engine internals; not implemented.

## Evidence

- `native/src/loader/runtime_gate.cpp:77-92` (`dl_iterate_phdr` match),
  `:336-380` (dynamic table walk, JUMP_SLOT/GLOB_DAT scan),
  `:121-145` (`mprotect` + cache flush),
  `:205-225` (`dlsym` forwarding and interception),
  `:147-173` (init wrapper, readiness notification).
- The gate log line `runtime gate installed on libunity.so (dlsym slot,
  base=...)` appears in the captured emulator log for the test app
  (`artifacts/emulator-20261118-191549`), with mod loading running afterward.
- The 1026/1025 values are the AArch64 constants from `<elf.h>`; the code uses
  the symbolic macros, not literals.

## Non-claims

- Only the tested `libunity.so` (Unity 6000.6.9f1, arm64-v8a) was exercised,
  via an x86_64 emulator running ARM translation.
- `mprotect` behavior on Quest's SELinux policy was unverified at the time of
  this ADR (see the Update below).
- No claim is made that the gate works on 32-bit or non-ELF configurations; the
  loader is built for 64-bit Android ABIs.

Update (2026-11-20): the gate was installed and exercised on a physical Meta
Quest 3 (Android 14, 4 KiB pages, non-root), at `libunity.so` base
`0x7835213000`, before IL2CPP initialization. This closes the Quest
`mprotect`/SELinux non-claim for that device and OS only; see
`docs/testing/results-quest-2026-11-20.md`.
