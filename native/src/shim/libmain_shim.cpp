// Replacement libmain.so.
//
// The stock Unity libmain.so receives the native library directory through
// NativeLoader.load(String) and dlopens <dir>/libunity.so followed by
// <dir>/libil2cpp.so, invoking each library's JNI_OnLoad. This shim performs
// the same steps and additionally loads libmocha.so, whose bootstrap starts the
// mod loader. Mocha never replaces or wraps any Unity library itself.

#include <jni.h>

#include <android/log.h>
#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

using MochaBootstrapFn = void (*)(JavaVM*);
using MochaNotifyFn = void (*)(const char*, void*);
using MochaShutdownFn = void (*)(void);

JavaVM* g_vm = nullptr;
void* g_mocha = nullptr;
MochaBootstrapFn g_bootstrap = nullptr;
MochaNotifyFn g_notify = nullptr;
MochaShutdownFn g_mocha_shutdown = nullptr;

void log_message(int priority, const char* message) {
    __android_log_write(priority, "Mocha", message ? message : "(null)");
    if (priority >= ANDROID_LOG_ERROR) {
        std::fprintf(stderr, "Mocha: %s\n", message ? message : "(null)");
    }
}

bool load_mocha() {
    if (g_mocha) {
        return true;
    }

    void* handle = dlopen("libmocha.so", RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(&JNI_OnLoad), &info) && info.dli_fname) {
            std::string path(info.dli_fname);
            const auto slash = path.find_last_of('/');
            if (slash != std::string::npos) {
                path = path.substr(0, slash + 1) + "libmocha.so";
                handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
            }
        }
    }

    if (!handle) {
        const char* error = dlerror();
        std::string message = "libmocha.so could not be loaded: ";
        message += error ? error : "unknown error";
        log_message(ANDROID_LOG_ERROR, message.c_str());
        return false;
    }

    g_mocha = handle;
    g_bootstrap = reinterpret_cast<MochaBootstrapFn>(dlsym(handle, "mocha_bootstrap"));
    g_notify = reinterpret_cast<MochaNotifyFn>(dlsym(handle, "mocha_notify_library_loaded"));
    g_mocha_shutdown = reinterpret_cast<MochaShutdownFn>(dlsym(handle, "mocha_shutdown"));

    if (!g_bootstrap) {
        log_message(ANDROID_LOG_ERROR, "libmocha.so does not export mocha_bootstrap");
        return false;
    }
    return true;
}

void* open_unity_library(const char* directory, const char* name) {
    char path[2048];
    const int written = std::snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(path)) {
        log_message(ANDROID_LOG_ERROR, "native library path is too long");
        return nullptr;
    }

    void* handle = dlopen(path, RTLD_LAZY);
    if (!handle) {
        handle = dlopen(name, RTLD_LAZY);
    }
    if (!handle) {
        const char* error = dlerror();
        std::string message = "unable to load ";
        message += path;
        message += ": ";
        message += error ? error : "unknown error";
        log_message(ANDROID_LOG_ERROR, message.c_str());
    }
    return handle;
}

bool invoke_jni_on_load(void* handle, const char* name) {
    using JniOnLoadFn = jint (*)(JavaVM*, void*);
    auto on_load = reinterpret_cast<JniOnLoadFn>(dlsym(handle, "JNI_OnLoad"));
    if (!on_load) {
        return true;
    }

    const jint version = on_load(g_vm, nullptr);
    if (version < JNI_VERSION_1_6) {
        std::string message = "JNI_OnLoad of ";
        message += name;
        message += " returned an unsupported version";
        log_message(ANDROID_LOG_ERROR, message.c_str());
        dlclose(handle);
        return false;
    }
    return true;
}

jboolean JNICALL native_load(JNIEnv* env, jclass, jstring java_path) {
    if (!java_path) {
        return JNI_FALSE;
    }

    const char* utf = env->GetStringUTFChars(java_path, nullptr);
    if (!utf) {
        return JNI_FALSE;
    }
    const std::string directory(utf);
    env->ReleaseStringUTFChars(java_path, utf);

    std::string message = "loading Unity libraries from ";
    message += directory;
    log_message(ANDROID_LOG_INFO, message.c_str());

    void* unity = open_unity_library(directory.c_str(), "libunity.so");
    if (unity) {
        // Report libunity before running its JNI_OnLoad: Mocha patches the
        // dlsym relocation in libunity so it can observe il2cpp_init before
        // any IL2CPP API call is attempted.
        if (g_notify) {
            g_notify("libunity.so", unity);
        }
        if (!invoke_jni_on_load(unity, "libunity.so")) {
            unity = nullptr;
        }
    }

    void* il2cpp = open_unity_library(directory.c_str(), "libil2cpp.so");
    if (il2cpp) {
        if (g_notify) {
            g_notify("libil2cpp.so", il2cpp);
        }
        invoke_jni_on_load(il2cpp, "libil2cpp.so");
    }

    return unity ? JNI_TRUE : JNI_FALSE;
}

jboolean JNICALL native_unload(JNIEnv*, jclass) {
    return JNI_TRUE;
}

const JNINativeMethod kNativeMethods[] = {
    {const_cast<char*>("load"), const_cast<char*>("(Ljava/lang/String;)Z"),
     reinterpret_cast<void*>(native_load)},
    {const_cast<char*>("unload"), const_cast<char*>("()Z"),
     reinterpret_cast<void*>(native_unload)},
};

} // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    g_vm = vm;

    if (load_mocha()) {
        g_bootstrap(vm);
    }

    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env) {
        log_message(ANDROID_LOG_ERROR, "unable to obtain JNIEnv in JNI_OnLoad");
        return JNI_ERR;
    }

    jclass native_loader = env->FindClass("com/unity3d/player/NativeLoader");
    if (!native_loader) {
        env->ExceptionClear();
        log_message(ANDROID_LOG_ERROR, "com.unity3d.player.NativeLoader not found");
        return JNI_ERR;
    }

    const jint result = env->RegisterNatives(
        native_loader, kNativeMethods,
        static_cast<jint>(sizeof(kNativeMethods) / sizeof(kNativeMethods[0])));
    env->DeleteLocalRef(native_loader);

    if (result != JNI_OK) {
        env->ExceptionClear();
        log_message(ANDROID_LOG_ERROR, "RegisterNatives failed");
        return JNI_ERR;
    }

    log_message(ANDROID_LOG_INFO, "libmain shim ready");
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* /*vm*/, void* /*reserved*/) {
    if (g_mocha_shutdown) {
        g_mocha_shutdown();
    }
    if (g_mocha) {
        dlclose(g_mocha);
        g_mocha = nullptr;
    }
}
