/*
 * Mocha mod ABI.
 *
 * This header is the stable contract between the Mocha loader and native mods.
 * It is pure C, has no C++ or Android/Unity headers, and may be included by
 * mods built with any toolchain.
 *
 * ABI rules:
 *   - MOCHA_ABI_VERSION changes only for incompatible layout/semantic changes.
 *   - MochaApi and MochaIl2CppApi are append-only. Readers must use struct_size
 *     before touching fields added after the ABI version they were built for.
 *   - All strings are UTF-8, NUL-terminated, and owned by Mocha unless stated.
 *   - A mod must not let C++ exceptions cross its exported boundary.
 */
#ifndef MOCHA_API_H
#define MOCHA_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOCHA_ABI_VERSION 1u

/* Symbol that Mocha looks up in a mod library when the manifest omits "entry". */
#define MOCHA_DEFAULT_ENTRY "mocha_mod_init"
/* Optional symbol called in reverse load order during a clean shutdown. */
#define MOCHA_DEFAULT_SHUTDOWN "mocha_mod_shutdown"
/* Optional symbol called after the managed runtime is fully ready. */
#define MOCHA_DEFAULT_RUNTIME_READY "mocha_mod_runtime_ready"

#ifdef __cplusplus
#define MOCHA_MOD_EXPORT extern "C" __attribute__((visibility("default")))
#else
#define MOCHA_MOD_EXPORT extern __attribute__((visibility("default")))
#endif

typedef enum MochaLogLevel {
    MOCHA_LOG_TRACE = 0,
    MOCHA_LOG_DEBUG = 1,
    MOCHA_LOG_INFO = 2,
    MOCHA_LOG_WARN = 3,
    MOCHA_LOG_ERROR = 4
} MochaLogLevel;

typedef void (*MochaLogFn)(MochaLogLevel level, const char* message);

/*
 * Subset of the IL2CPP embedding API.
 *
 * Opaque pointers map to the corresponding IL2CPP types:
 *   domain   -> Il2CppDomain*
 *   assembly -> const Il2CppAssembly*
 *   image    -> const Il2CppImage*
 *   klass    -> Il2CppClass*
 *   method   -> const MethodInfo*
 *   field    -> FieldInfo*
 *   object   -> Il2CppObject*
 *   string   -> Il2CppString*
 *   exception-> Il2CppException*
 *
 * A pointer is NULL when the running libil2cpp.so did not export that function.
 * Always check before use. Mocha never guesses at missing symbols.
 */
typedef struct MochaIl2CppApi {
    uint32_t struct_size;

    void*       (*domain_get)(void);
    void*       (*thread_attach)(void* domain);
    void        (*thread_detach)(void* thread);
    const void* const* (*domain_get_assemblies)(const void* domain, size_t* count);
    const void* (*assembly_get_image)(const void* assembly);
    void*       (*class_from_name)(const void* image, const char* namespaze, const char* name);
    const void* (*class_get_method_from_name)(void* klass, const char* name, int argc);
    void*       (*runtime_invoke)(const void* method, void* object, void** params,
                                  void** exception_out);
    void*       (*string_new)(const char* utf8);
    const uint16_t* (*string_chars)(void* string);
    int32_t     (*string_length)(void* string);
    void*       (*object_new)(void* klass);
    void*       (*object_unbox)(void* object);
    void*       (*object_get_class)(void* object);
    void*       (*class_get_field_from_name)(void* klass, const char* name);
    void        (*field_static_get_value)(void* field, void* value_out);
    void        (*field_static_set_value)(void* field, const void* value);
    void        (*field_get_value)(void* object, void* field, void* value_out);
    void        (*field_set_value)(void* object, void* field, const void* value);
    size_t      (*field_get_offset)(void* field);
    const char* (*image_get_name)(const void* image);
    const char* (*class_get_name)(void* klass);
    const char* (*class_get_namespace)(void* klass);
    const char* (*method_get_name)(const void* method);
    uint32_t    (*method_get_param_count)(const void* method);
    void        (*gc_disable)(void);
    void        (*gc_enable)(void);
} MochaIl2CppApi;

/*
 * Passed to a mod's entry point. Mocha owns this structure and every string in
 * it; mods must copy any value they need after init returns.
 */
typedef struct MochaApi {
    uint32_t abi_version;
    uint32_t struct_size;
    const char* mocha_version;
    const char* unity_version;      /* may be NULL if it could not be resolved */
    const char* android_abi;        /* "arm64-v8a", "x86_64", ... */
    int32_t     android_api_level;  /* 0 if unknown */
    const char* data_directory;     /* absolute; Mocha-owned root */
    const char* mods_directory;     /* absolute */
    MochaLogFn  log;
    const MochaIl2CppApi* il2cpp;
} MochaApi;

/*
 * Entry point contract.
 * Return 0 on success. Any other value marks the mod as failed; Mocha logs it
 * and keeps the process and other mods alive. The API pointer is valid until
 * the mod returns from shutdown (or for the process lifetime if it has none).
 */
typedef int (*MochaModInitFn)(const MochaApi* api);
typedef int (*MochaModRuntimeReadyFn)(const MochaApi* api);
typedef void (*MochaModShutdownFn)(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MOCHA_API_H */
