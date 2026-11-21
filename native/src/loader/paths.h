#ifndef MOCHA_PATHS_H
#define MOCHA_PATHS_H

#include <jni.h>

#include <string>

namespace mocha {

std::string jstring_to_utf8(JNIEnv* env, jstring value);
std::string resolve_files_root(JNIEnv* env);
std::string resolve_internal_files_root(JNIEnv* env);

} // namespace mocha

#endif
