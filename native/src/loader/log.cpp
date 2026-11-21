#include "log.h"

#include <android/log.h>

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <sys/stat.h>

namespace mocha {
namespace {

std::mutex g_mutex;
std::string g_path;
FILE* g_file = nullptr;
constexpr long kMaxLogBytes = 1024 * 1024;

const char* level_name(MochaLogLevel level) {
    switch (level) {
    case MOCHA_LOG_TRACE: return "T";
    case MOCHA_LOG_DEBUG: return "D";
    case MOCHA_LOG_INFO: return "I";
    case MOCHA_LOG_WARN: return "W";
    case MOCHA_LOG_ERROR: return "E";
    }
    return "?";
}

int android_priority(MochaLogLevel level) {
    switch (level) {
    case MOCHA_LOG_TRACE: return ANDROID_LOG_VERBOSE;
    case MOCHA_LOG_DEBUG: return ANDROID_LOG_DEBUG;
    case MOCHA_LOG_INFO: return ANDROID_LOG_INFO;
    case MOCHA_LOG_WARN: return ANDROID_LOG_WARN;
    case MOCHA_LOG_ERROR: return ANDROID_LOG_ERROR;
    }
    return ANDROID_LOG_DEFAULT;
}

void rotate_if_needed(const std::string& path) {
    struct stat info {};
    if (stat(path.c_str(), &info) != 0 || info.st_size < kMaxLogBytes) {
        return;
    }
    const std::string backup = path + ".1";
    std::remove(backup.c_str());
    std::rename(path.c_str(), backup.c_str());
}

void write_locked(MochaLogLevel level, const char* message) {
    __android_log_write(android_priority(level), "Mocha", message);

    if (!g_file) {
        return;
    }

    const std::time_t now = std::time(nullptr);
    std::tm utc {};
    gmtime_r(&now, &utc);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    std::fprintf(g_file, "%s %s %s\n", stamp, level_name(level), message);
    std::fflush(g_file);
}

} // namespace

void log_init(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_path = path;
    rotate_if_needed(path);
    g_file = std::fopen(path.c_str(), "a");
    if (!g_file) {
        __android_log_print(ANDROID_LOG_WARN, "Mocha", "cannot open log file %s", path.c_str());
    }
}

void log_shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        std::fflush(g_file);
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void log_write(MochaLogLevel level, const char* message) {
    std::lock_guard<std::mutex> lock(g_mutex);
    write_locked(level, message ? message : "(null)");
}

void log_write(MochaLogLevel level, const std::string& message) {
    log_write(level, message.c_str());
}

void log_printf(MochaLogLevel level, const char* format, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log_write(level, buffer);
}

} // namespace mocha
