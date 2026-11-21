#include "il2cpp_api.h"
#include "log.h"
#include "manifest.h"
#include "paths.h"
#include "plan.h"
#include "runtime_gate.h"

#include <mocha/mocha_api.h>

#include <jni.h>

#include <android/log.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#define MOCHA_EXPORT __attribute__((visibility("default")))

namespace fs = std::filesystem;

namespace mocha {
namespace {

constexpr const char* kLoaderVersion = "0.1.0";
constexpr int kRuntimeTimeoutSeconds = 60;

JavaVM* g_vm = nullptr;
std::mutex g_state_mutex;
void* g_il2cpp_handle = nullptr;
std::atomic<bool> g_started {false};
std::atomic<bool> g_stopping {false};

std::string g_data_root;
std::string g_internal_root;
std::string g_mods_dir;
std::string g_unity_version;
MochaApi g_public_api {};

std::mutex g_loaded_mutex;
std::vector<std::pair<void*, MochaModShutdownFn>> g_loaded;

std::string android_abi_name() {
#if defined(__aarch64__)
    return "arm64-v8a";
#elif defined(__arm__)
    return "armeabi-v7a";
#elif defined(__x86_64__)
    return "x86_64";
#elif defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

int device_api_level() {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("ro.build.version.sdk", value) <= 0) {
        return 0;
    }
    return std::atoi(value);
}

const std::string& abi_name() {
    static const std::string name = android_abi_name();
    return name;
}

JNIEnv* obtain_env(bool& attached) {
    attached = false;
    if (!g_vm) {
        return nullptr;
    }
    JNIEnv* env = nullptr;
    const jint result = g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (result == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            return nullptr;
        }
        attached = true;
        return env;
    }
    if (result != JNI_OK) {
        return nullptr;
    }
    return env;
}

std::string utf16_to_utf8(const uint16_t* chars, int32_t length) {
    std::string out;
    if (!chars || length <= 0) {
        return out;
    }
    out.reserve(static_cast<size_t>(length));
    for (int32_t i = 0; i < length; ++i) {
        uint32_t code = chars[i];
        if (code >= 0xD800 && code <= 0xDBFF && i + 1 < length) {
            const uint32_t low = chars[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return out;
}

void mod_log(MochaLogLevel level, const char* message) {
    log_write(level, message ? message : "(null)");
}

void* find_image(const char* assembly_name) {
    const MochaIl2CppApi* api = il2cpp_api();
    if (!api->domain_get || !api->domain_get_assemblies || !api->assembly_get_image ||
        !api->image_get_name) {
        return nullptr;
    }
    void* domain = api->domain_get();
    if (!domain) {
        return nullptr;
    }
    size_t count = 0;
    const void* const* assemblies = api->domain_get_assemblies(domain, &count);
    if (!assemblies) {
        return nullptr;
    }
    for (size_t i = 0; i < count; ++i) {
        const void* image = api->assembly_get_image(assemblies[i]);
        const char* name = image ? api->image_get_name(image) : nullptr;
        if (name && std::strcmp(name, assembly_name) == 0) {
            return const_cast<void*>(image);
        }
    }
    return nullptr;
}

std::string try_resolve_unity_version() {
    const MochaIl2CppApi* api = il2cpp_api();
    if (!api->class_from_name || !api->class_get_method_from_name || !api->runtime_invoke ||
        !api->string_chars || !api->string_length) {
        return {};
    }
    void* image = find_image("UnityEngine.CoreModule.dll");
    if (!image) {
        return {};
    }
    void* klass = api->class_from_name(image, "UnityEngine", "Application");
    if (!klass) {
        return {};
    }
    const void* method = api->class_get_method_from_name(klass, "get_unityVersion", 0);
    if (!method) {
        return {};
    }
    void* exception = nullptr;
    void* result = api->runtime_invoke(method, nullptr, nullptr, &exception);
    if (!result || exception) {
        return {};
    }
    const int32_t length = api->string_length(result);
    const uint16_t* chars = api->string_chars(result);
    return utf16_to_utf8(chars, length);
}

void* wait_for_runtime() {
    const MochaIl2CppApi* api = il2cpp_api();
    if (!runtime_gate_installed()) {
        log_write(MOCHA_LOG_ERROR,
                  "the libunity runtime gate is not installed; refusing to call IL2CPP "
                  "before il2cpp_init");
        return nullptr;
    }

    const auto timeout = std::chrono::seconds(kRuntimeTimeoutSeconds);
    if (!wait_for_il2cpp_init(timeout)) {
        log_write(MOCHA_LOG_ERROR,
                  "timed out waiting for il2cpp_init; the loader will not call IL2CPP");
        return nullptr;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!g_stopping && std::chrono::steady_clock::now() < deadline) {
        void* domain = api->domain_get ? api->domain_get() : nullptr;
        if (domain && api->domain_get_assemblies) {
            size_t count = 0;
            const void* const* assemblies = api->domain_get_assemblies(domain, &count);
            if (assemblies && count > 0) {
                return domain;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    log_write(MOCHA_LOG_ERROR, "timed out waiting for managed assemblies to appear");
    return nullptr;
}

void* open_mod_library(const Manifest& manifest, std::string& error) {
    void* handle = dlopen(manifest.library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle) {
        return handle;
    }

    const char* original_error = dlerror();
    std::string original = original_error ? original_error : "unknown dlopen error";

    std::error_code code;
    // Android's app linker namespace only permits loading code from app-internal
    // storage, so mods discovered under external storage are staged here first.
    const std::string staging_root = g_internal_root.empty() ? g_data_root : g_internal_root;
    const std::string internal_dir = staging_root + "/mocha/native";
    fs::create_directories(internal_dir, code);
    const std::string target =
        internal_dir + "/" + fs::path(manifest.library_path).stem().string() + ".so";
    fs::copy_file(manifest.library_path, target, fs::copy_options::overwrite_existing, code);
    if (code) {
        error = "dlopen failed (" + original + "); copying to internal storage failed: " +
                code.message();
        return nullptr;
    }
    ::chmod(target.c_str(), 0755);

    handle = dlopen(target.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle) {
        log_printf(MOCHA_LOG_INFO, "mod %s loaded from internal copy", manifest.id.c_str());
        return handle;
    }
    const char* fallback_error = dlerror();
    error = "dlopen failed (" + original + "); internal copy failed: " +
            (fallback_error ? fallback_error : "unknown error");
    return nullptr;
}

void write_summary(const std::string& path, const std::vector<std::string>& lines) {
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        log_printf(MOCHA_LOG_ERROR, "cannot write summary file %s", path.c_str());
        return;
    }
    for (const auto& line : lines) {
        file << line << '\n';
    }
}

std::vector<Manifest> discover_mods(std::vector<std::string>& errors) {
    std::vector<Manifest> mods;
    std::error_code code;
    if (!fs::is_directory(g_mods_dir, code)) {
        fs::create_directories(g_mods_dir, code);
        if (code) {
            errors.push_back("mods directory cannot be created: " + code.message());
            return mods;
        }
    }

    std::vector<fs::path> manifests;
    for (const auto& entry : fs::directory_iterator(g_mods_dir, code)) {
        if (code) {
            errors.push_back("mods directory cannot be read: " + code.message());
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".json") {
            continue;
        }
        manifests.push_back(entry.path());
    }
    std::sort(manifests.begin(), manifests.end());

    for (const auto& path : manifests) {
        std::ifstream file(path);
        if (!file) {
            errors.push_back(path.filename().string() + ": cannot be opened");
            continue;
        }
        const std::string text((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());

        Manifest manifest;
        std::string error;
        if (!parse_manifest(text, manifest, error)) {
            errors.push_back(path.filename().string() + ": " + error);
            continue;
        }

        manifest.manifest_path = path.string();
        manifest.library_path = (path.parent_path() / (path.stem().string() + ".so")).string();
        if (!fs::is_regular_file(manifest.library_path, code)) {
            errors.push_back(manifest.id + ": library not found (" +
                             fs::path(manifest.library_path).filename().string() + ")");
            continue;
        }
        mods.push_back(std::move(manifest));
    }

    return mods;
}

// Runs on the thread that called il2cpp_init(): the gate invokes this callback
// after the real initializer returns and before libunity regains control.
// Executing mod initialization here keeps all managed calls on Unity's own
// IL2CPP init thread instead of a detached worker created mid-startup.
void run_mod_loading() {
    void* handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        handle = g_il2cpp_handle;
    }
    if (!handle) {
        log_write(MOCHA_LOG_ERROR, "libil2cpp.so was never reported; cannot load mods");
        return;
    }

    if (!resolve_il2cpp_api(handle)) {
        log_write(MOCHA_LOG_ERROR, "required IL2CPP API symbols are missing");
        return;
    }
    const MochaIl2CppApi* api = il2cpp_api();
    if (il2cpp_missing_symbols() > 0) {
        log_printf(MOCHA_LOG_WARN, "%zu optional IL2CPP symbols are unavailable",
                   il2cpp_missing_symbols());
    }
    log_write(MOCHA_LOG_INFO, "IL2CPP API resolved");

    void* domain = wait_for_runtime();
    if (!domain) {
        log_write(MOCHA_LOG_ERROR, "timed out waiting for the IL2CPP runtime");
        return;
    }
    {
        size_t assembly_count = 0;
        const void* const* assemblies = api->domain_get_assemblies(domain, &assembly_count);
        log_printf(MOCHA_LOG_INFO, "IL2CPP runtime is ready (%zu assemblies visible%s)",
                   assembly_count, assemblies ? "" : ", list unavailable");
    }

    void* thread = api->thread_attach ? api->thread_attach(domain) : nullptr;
    if (!thread) {
        log_write(MOCHA_LOG_WARN, "mod thread could not be attached to the IL2CPP domain");
    }

    g_unity_version = try_resolve_unity_version();
    if (!g_unity_version.empty()) {
        log_printf(MOCHA_LOG_INFO, "Unity version %s", g_unity_version.c_str());
    }

    g_public_api.abi_version = MOCHA_ABI_VERSION;
    g_public_api.struct_size = static_cast<uint32_t>(sizeof(MochaApi));
    g_public_api.mocha_version = kLoaderVersion;
    g_public_api.unity_version = g_unity_version.empty() ? nullptr : g_unity_version.c_str();
    g_public_api.android_abi = abi_name().c_str();
    g_public_api.android_api_level = device_api_level();
    g_public_api.data_directory = g_data_root.c_str();
    g_public_api.mods_directory = g_mods_dir.c_str();
    g_public_api.log = &mod_log;
    g_public_api.il2cpp = api;

    std::vector<std::string> discovery_errors;
    std::vector<Manifest> manifests = discover_mods(discovery_errors);
    for (const auto& error : discovery_errors) {
        log_printf(MOCHA_LOG_WARN, "mod rejected during discovery: %s", error.c_str());
    }

    ResolveResult resolved = resolve_mods(std::move(manifests));
    for (const auto& error : resolved.errors) {
        log_printf(MOCHA_LOG_WARN, "mod skipped: %s", error.c_str());
    }

    const size_t discovered = resolved.mods.size();
    const size_t skipped = discovered - resolved.load_order.size();
    size_t loaded = 0;
    size_t failed = 0;
    std::vector<std::string> summary;
    summary.push_back("loader_version=" + std::string(kLoaderVersion));
    summary.push_back("abi=" + android_abi_name());
    summary.push_back("unity=" + (g_unity_version.empty() ? std::string("unknown") : g_unity_version));
    summary.push_back("discovered=" + std::to_string(discovered));
    summary.push_back("skipped=" + std::to_string(skipped));
    for (const auto& error : discovery_errors) {
        summary.push_back("rejected=" + error);
    }
    for (const auto& error : resolved.errors) {
        summary.push_back("skipped_mod=" + error);
    }

    for (const auto& manifest : resolved.load_order) {
        if (g_stopping) {
            break;
        }

        std::string error;
        void* mod_handle = open_mod_library(manifest, error);
        if (!mod_handle) {
            log_printf(MOCHA_LOG_ERROR, "mod %s failed to load: %s", manifest.id.c_str(),
                       error.c_str());
            summary.push_back("mod." + manifest.id + "=failed:dlopen");
            ++failed;
            continue;
        }

        auto entry = reinterpret_cast<MochaModInitFn>(dlsym(mod_handle, manifest.entry.c_str()));
        if (!entry) {
            log_printf(MOCHA_LOG_ERROR, "mod %s does not export %s", manifest.id.c_str(),
                       manifest.entry.c_str());
            summary.push_back("mod." + manifest.id + "=failed:entry-missing");
            ++failed;
            continue;
        }

        int result = 1;
        log_printf(MOCHA_LOG_INFO, "invoking %s for %s", manifest.entry.c_str(),
                   manifest.id.c_str());
        try {
            result = entry(&g_public_api);
        } catch (const std::exception& exception) {
            log_printf(MOCHA_LOG_ERROR, "mod %s raised an exception: %s", manifest.id.c_str(),
                       exception.what());
            result = 1;
        } catch (...) {
            log_printf(MOCHA_LOG_ERROR, "mod %s raised an unknown exception", manifest.id.c_str());
            result = 1;
        }

        if (result != 0) {
            log_printf(MOCHA_LOG_ERROR, "mod %s init returned %d", manifest.id.c_str(), result);
            summary.push_back("mod." + manifest.id + "=failed:init");
            ++failed;
            continue;
        }

        auto ready = reinterpret_cast<MochaModRuntimeReadyFn>(
            dlsym(mod_handle, MOCHA_DEFAULT_RUNTIME_READY));
        if (ready) {
            try {
                if (ready(&g_public_api) != 0) {
                    log_printf(MOCHA_LOG_WARN, "mod %s runtime-ready hook returned an error",
                               manifest.id.c_str());
                }
            } catch (...) {
                log_printf(MOCHA_LOG_WARN, "mod %s runtime-ready hook raised an exception",
                           manifest.id.c_str());
            }
        }

        auto shutdown =
            reinterpret_cast<MochaModShutdownFn>(dlsym(mod_handle, manifest.shutdown.c_str()));
        {
            std::lock_guard<std::mutex> lock(g_loaded_mutex);
            g_loaded.emplace_back(mod_handle, shutdown);
        }

        log_printf(MOCHA_LOG_INFO, "mod loaded: %s %s", manifest.id.c_str(),
                   manifest.version.c_str());
        summary.push_back("mod." + manifest.id + "=loaded");
        ++loaded;
    }

    summary.push_back("loaded=" + std::to_string(loaded));
    summary.push_back("failed=" + std::to_string(failed));
    write_summary(g_data_root + "/mocha/loader-summary.txt", summary);

    log_printf(MOCHA_LOG_INFO,
               "mod load complete: discovered=%zu loaded=%zu failed=%zu skipped=%zu",
               discovered, loaded, failed, skipped);
}

} // namespace
} // namespace mocha

extern "C" MOCHA_EXPORT void mocha_bootstrap(JavaVM* vm) {
    using namespace mocha;
    if (g_started.exchange(true)) {
        return;
    }
    g_vm = vm;

    bool attached = false;
    JNIEnv* env = obtain_env(attached);
    if (!env) {
        __android_log_write(ANDROID_LOG_ERROR, "Mocha", "cannot obtain JNIEnv");
        return;
    }

    g_data_root = resolve_files_root(env);
    g_internal_root = resolve_internal_files_root(env);
    if (attached) {
        g_vm->DetachCurrentThread();
    }
    if (g_data_root.empty()) {
        __android_log_write(ANDROID_LOG_ERROR, "Mocha", "cannot resolve the data directory");
        return;
    }

    g_mods_dir = g_data_root + "/mocha/mods";
    std::error_code code;
    fs::create_directories(g_mods_dir, code);
    fs::create_directories(g_data_root + "/mocha", code);

    log_init(g_data_root + "/mocha/mocha.log");
    log_printf(MOCHA_LOG_INFO, "Mocha %s bootstrapping (abi=%s, api=%d, data=%s)",
               kLoaderVersion, android_abi_name().c_str(), device_api_level(),
               g_data_root.c_str());

    // Mod discovery and initialization run from the runtime gate once libunity's
    // il2cpp_init lookup succeeds, so every managed call happens on the engine's
    // own init thread.
    set_runtime_ready_callback(&run_mod_loading);
    log_write(MOCHA_LOG_INFO, "waiting for the IL2CPP runtime gate to reach il2cpp_init");
}

extern "C" MOCHA_EXPORT void mocha_notify_library_loaded(const char* name, void* handle) {
    using namespace mocha;
    if (!name) {
        return;
    }
    if (std::strcmp(name, "libunity.so") == 0) {
        std::string error;
        if (install_runtime_gate(handle, error)) {
            log_write(MOCHA_LOG_INFO,
                      "IL2CPP runtime gate installed before libunity initializes IL2CPP");
        } else {
            log_printf(MOCHA_LOG_ERROR, "IL2CPP runtime gate unavailable: %s", error.c_str());
        }
        return;
    }
    if (std::strcmp(name, "libil2cpp.so") != 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_il2cpp_handle = handle;
    }
    log_write(MOCHA_LOG_INFO, "libil2cpp.so reported by the libmain shim");
}

extern "C" MOCHA_EXPORT const char* mocha_loader_version(void) {
    return mocha::kLoaderVersion;
}

extern "C" MOCHA_EXPORT void mocha_shutdown(void) {
    using namespace mocha;
    if (g_stopping.exchange(true)) {
        return;
    }

    std::vector<std::pair<void*, MochaModShutdownFn>> loaded;
    {
        std::lock_guard<std::mutex> lock(g_loaded_mutex);
        loaded = g_loaded;
    }

    for (auto it = loaded.rbegin(); it != loaded.rend(); ++it) {
        if (!it->second) {
            continue;
        }
        try {
            it->second();
        } catch (...) {
            log_write(MOCHA_LOG_ERROR, "mod shutdown raised an exception");
        }
    }
    log_write(MOCHA_LOG_INFO, "Mocha shutdown complete");
    log_shutdown();
}
