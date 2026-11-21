#ifndef MOCHA_LOG_H
#define MOCHA_LOG_H

#include <mocha/mocha_api.h>

#include <cstddef>
#include <string>

namespace mocha {

void log_init(const std::string& path);
void log_shutdown();
void log_write(MochaLogLevel level, const char* message);
void log_write(MochaLogLevel level, const std::string& message);
void log_printf(MochaLogLevel level, const char* format, ...);

} // namespace mocha

#endif
