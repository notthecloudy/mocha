#include "paths.h"

#include "log.h"

#include <cstdio>
#include <filesystem>
#include <system_error>
#include <unistd.h>

namespace mocha {
namespace {

jobject current_application(JNIEnv* env) {
    jclass activity_thread = env->FindClass("android/app/ActivityThread");
    if (!activity_thread) {
        env->ExceptionClear();
        return nullptr;
    }

    jmethodID current = env->GetStaticMethodID(
        activity_thread, "currentApplication", "()Landroid/app/Application;");
    if (!current) {
        env->ExceptionClear();
        env->DeleteLocalRef(activity_thread);
        return nullptr;
    }

    jobject application = env->CallStaticObjectMethod(activity_thread, current);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        application = nullptr;
    }
    env->DeleteLocalRef(activity_thread);
    return application;
}

std::string file_absolute_path(JNIEnv* env, jobject file) {
    if (!file) {
        return {};
    }
    jclass file_class = env->GetObjectClass(file);
    if (!file_class) {
        env->ExceptionClear();
        return {};
    }
    jmethodID absolute = env->GetMethodID(file_class, "getAbsolutePath", "()Ljava/lang/String;");
    env->DeleteLocalRef(file_class);
    if (!absolute) {
        env->ExceptionClear();
        return {};
    }
    auto path = static_cast<jstring>(env->CallObjectMethod(file, absolute));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return {};
    }
    std::string result = jstring_to_utf8(env, path);
    if (path) {
        env->DeleteLocalRef(path);
    }
    return result;
}

std::string external_files_root(JNIEnv* env, jclass context_class, jobject application) {
    jmethodID external = env->GetMethodID(
        context_class, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;");
    if (!external) {
        env->ExceptionClear();
        return {};
    }

    jobject file = env->CallObjectMethod(application, external, nullptr);
    std::string result;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    } else {
        result = file_absolute_path(env, file);
    }
    if (file) {
        env->DeleteLocalRef(file);
    }
    return result;
}

std::string internal_files_root(JNIEnv* env, jclass context_class, jobject application) {
    jmethodID internal = env->GetMethodID(context_class, "getFilesDir", "()Ljava/io/File;");
    if (!internal) {
        env->ExceptionClear();
        return {};
    }

    jobject file = env->CallObjectMethod(application, internal);
    std::string result;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    } else {
        result = file_absolute_path(env, file);
    }
    if (file) {
        env->DeleteLocalRef(file);
    }
    return result;
}

bool writable_directory(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    const std::string probe = path + "/.mocha-write-probe";
    FILE* file = std::fopen(probe.c_str(), "we");
    if (!file) {
        return false;
    }
    std::fclose(file);
    ::unlink(probe.c_str());
    return true;
}

bool prepare_root(const std::string& root) {
    if (root.empty()) {
        return false;
    }
    std::error_code code;
    const std::string mocha = root + "/mocha";
    std::filesystem::create_directories(mocha, code);
    if (code) {
        return false;
    }
    return writable_directory(mocha);
}

} // namespace

std::string jstring_to_utf8(JNIEnv* env, jstring value) {
    if (!value) {
        return {};
    }
    const char* utf = env->GetStringUTFChars(value, nullptr);
    if (!utf) {
        env->ExceptionClear();
        return {};
    }
    std::string result(utf);
    env->ReleaseStringUTFChars(value, utf);
    return result;
}

std::string resolve_internal_files_root(JNIEnv* env) {
    jobject application = current_application(env);
    if (!application) {
        log_write(MOCHA_LOG_ERROR, "cannot resolve the current Application object");
        return {};
    }

    jclass context_class = env->GetObjectClass(application);
    if (!context_class) {
        env->ExceptionClear();
        env->DeleteLocalRef(application);
        return {};
    }

    std::string result = internal_files_root(env, context_class, application);
    env->DeleteLocalRef(context_class);
    env->DeleteLocalRef(application);
    return result;
}

std::string resolve_files_root(JNIEnv* env) {
    jobject application = current_application(env);
    if (!application) {
        log_write(MOCHA_LOG_ERROR, "cannot resolve the current Application object");
        return {};
    }

    jclass context_class = env->GetObjectClass(application);
    if (!context_class) {
        env->ExceptionClear();
        env->DeleteLocalRef(application);
        return {};
    }

    const std::string external = external_files_root(env, context_class, application);
    const std::string internal = internal_files_root(env, context_class, application);
    env->DeleteLocalRef(context_class);
    env->DeleteLocalRef(application);

    std::string result;
    if (prepare_root(external)) {
        result = external;
    } else {
        if (!external.empty()) {
            log_printf(MOCHA_LOG_WARN,
                       "external files directory %s is not writable by the app; using internal "
                       "storage",
                       external.c_str());
        }
        if (prepare_root(internal)) {
            result = internal;
        }
    }
    if (result.empty()) {
        log_write(MOCHA_LOG_ERROR, "no usable files directory was returned by the Application");
        return {};
    }

    log_printf(MOCHA_LOG_INFO, "files root: %s", result.c_str());
    return result;
}

} // namespace mocha
