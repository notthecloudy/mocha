#ifndef MOCHA_RUNTIME_GATE_H
#define MOCHA_RUNTIME_GATE_H

#include <chrono>
#include <string>

namespace mocha {

// Installs a relocation-level interposer for dlsym() inside libunity.so.
//
// libunity resolves every IL2CPP entry point through
// UnityClassic::Baselib_DynamicLibrary_GetFunction(), which is a thin wrapper
// around dlsym(handle, name). Interposing the exported symbol from libmocha is
// not possible because the lookup is performed on the libil2cpp handle, which
// only resolves symbols from that library. Patching the JUMP_SLOT relocation
// for dlsym inside libunity is library-agnostic and allows Mocha to observe
// when il2cpp_init or il2cpp_init_utf16 is resolved and invoked.
//
// The gate must be installed after libunity.so has been loaded (its program
// headers are needed) but before libunity runs the code that resolves IL2CPP,
// which is why the libmain shim reports libunity right after dlopen.
bool install_runtime_gate(void* unity_handle, std::string& error);

bool runtime_gate_installed();

// Registers a callback invoked once, synchronously, on the thread that called
// il2cpp_init/il2cpp_init_utf16 immediately after the real initializer returns
// and before the result is handed back to libunity. Running mod initialization
// here means it executes on Unity's own IL2CPP init thread (the Java main
// thread) rather than on a detached worker, matching how the engine expects
// managed code to be called during startup.
void set_runtime_ready_callback(void (*callback)());

// Blocks until the wrapped il2cpp_init/il2cpp_init_utf16 has returned a
// non-null domain, or the timeout elapses. Never calls into IL2CPP.
bool wait_for_il2cpp_init(std::chrono::milliseconds timeout);

} // namespace mocha

#endif
