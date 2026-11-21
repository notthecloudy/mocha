#include "il2cpp_api.h"

#include <dlfcn.h>

#include <cstring>

namespace mocha {
namespace {

#define MOCHA_IL2CPP_FUNCTIONS(X)                 \
    X(domain_get)                                 \
    X(thread_attach)                              \
    X(thread_detach)                              \
    X(domain_get_assemblies)                      \
    X(assembly_get_image)                         \
    X(class_from_name)                            \
    X(class_get_method_from_name)                 \
    X(runtime_invoke)                             \
    X(string_new)                                 \
    X(string_chars)                               \
    X(string_length)                              \
    X(object_new)                                 \
    X(object_unbox)                               \
    X(object_get_class)                           \
    X(class_get_field_from_name)                  \
    X(field_static_get_value)                     \
    X(field_static_set_value)                     \
    X(field_get_value)                            \
    X(field_set_value)                            \
    X(field_get_offset)                           \
    X(image_get_name)                             \
    X(class_get_name)                             \
    X(class_get_namespace)                        \
    X(method_get_name)                            \
    X(method_get_param_count)                     \
    X(gc_disable)                                 \
    X(gc_enable)

MochaIl2CppApi g_api {};
size_t g_missing = 0;

} // namespace

bool resolve_il2cpp_api(void* library_handle) {
    std::memset(&g_api, 0, sizeof(g_api));
    g_api.struct_size = static_cast<uint32_t>(sizeof(g_api));
    g_missing = 0;

    auto resolve = [library_handle](const char* symbol) -> void* {
        void* address = library_handle ? dlsym(library_handle, symbol) : nullptr;
        if (!address) {
            address = dlsym(RTLD_DEFAULT, symbol);
        }
        return address;
    };

#define MOCHA_RESOLVE(name)                                                    \
    g_api.name = reinterpret_cast<decltype(g_api.name)>(resolve("il2cpp_" #name)); \
    if (!g_api.name) {                                                         \
        ++g_missing;                                                           \
    }

    MOCHA_IL2CPP_FUNCTIONS(MOCHA_RESOLVE)
#undef MOCHA_RESOLVE

    return g_api.domain_get && g_api.thread_attach && g_api.domain_get_assemblies &&
           g_api.assembly_get_image && g_api.class_from_name &&
           g_api.class_get_method_from_name && g_api.runtime_invoke;
}

const MochaIl2CppApi* il2cpp_api() {
    return &g_api;
}

size_t il2cpp_missing_symbols() {
    return g_missing;
}

} // namespace mocha
