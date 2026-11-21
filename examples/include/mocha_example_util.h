/*
 * Small helper layer shared by the Mocha example mods.
 *
 * It intentionally demonstrates the public Mocha API only: every IL2CPP entry
 * point is reached through MochaApi::il2cpp and is checked for NULL first.
 */
#ifndef MOCHA_EXAMPLE_UTIL_H
#define MOCHA_EXAMPLE_UTIL_H

#include <mocha/mocha_api.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace mocha_example {

inline void log(const MochaApi* mocha, MochaLogLevel level, const std::string& message) {
    if (mocha && mocha->log) {
        mocha->log(level, message.c_str());
    }
}

inline void* find_image(const MochaApi* mocha, const char* assembly_name) {
    const MochaIl2CppApi* api = mocha->il2cpp;
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

inline void* find_class(const MochaApi* mocha, const char* assembly_name,
                        const char* namespace_name, const char* class_name) {
    const MochaIl2CppApi* api = mocha->il2cpp;
    if (!api->class_from_name) {
        return nullptr;
    }
    void* image = find_image(mocha, assembly_name);
    if (!image) {
        return nullptr;
    }
    return api->class_from_name(image, namespace_name, class_name);
}

inline const void* find_method(const MochaApi* mocha, void* klass, const char* name, int argc) {
    const MochaIl2CppApi* api = mocha->il2cpp;
    if (!api->class_get_method_from_name || !klass) {
        return nullptr;
    }
    return api->class_get_method_from_name(klass, name, argc);
}

inline bool invoke_int(const MochaApi* mocha, const void* method, void** params,
                       int32_t& result) {
    const MochaIl2CppApi* api = mocha->il2cpp;
    if (!api->runtime_invoke || !api->object_unbox) {
        return false;
    }
    void* exception = nullptr;
    void* boxed = api->runtime_invoke(method, nullptr, params, &exception);
    if (!boxed || exception) {
        return false;
    }
    const int32_t* value = static_cast<const int32_t*>(api->object_unbox(boxed));
    if (!value) {
        return false;
    }
    result = *value;
    return true;
}

inline std::string utf16_to_utf8(const uint16_t* chars, int32_t length) {
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

inline bool invoke_string(const MochaApi* mocha, const void* method, void** params,
                          std::string& result) {
    const MochaIl2CppApi* api = mocha->il2cpp;
    if (!api->runtime_invoke || !api->string_chars || !api->string_length) {
        return false;
    }
    void* exception = nullptr;
    void* object = api->runtime_invoke(method, nullptr, params, &exception);
    if (!object || exception) {
        return false;
    }
    result = utf16_to_utf8(api->string_chars(object), api->string_length(object));
    return true;
}

inline bool get_static_int(const MochaApi* mocha, void* klass, const char* name,
                           int32_t& result) {
    const MochaIl2CppApi* api = mocha->il2cpp;
    if (!api->class_get_field_from_name || !api->field_static_get_value) {
        return false;
    }
    void* field = api->class_get_field_from_name(klass, name);
    if (!field) {
        return false;
    }
    api->field_static_get_value(field, &result);
    return true;
}

inline bool write_result(const MochaApi* mocha, const char* file_name,
                         const std::string& content) {
    const std::string path = std::string(mocha->data_directory) + "/mocha/" + file_name;
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        log(mocha, MOCHA_LOG_ERROR, "cannot write " + path);
        return false;
    }
    file << content << '\n';
    return true;
}

} // namespace mocha_example

#endif
