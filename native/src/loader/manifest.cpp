#include "manifest.h"

#include <mocha/mocha_api.h>

#include <nlohmann/json.hpp>

#include <cctype>

namespace mocha {
namespace {

using Json = nlohmann::json;

bool require_string(const Json& object, const char* key, bool required,
                    std::string& out, std::string& error) {
    const auto it = object.find(key);
    if (it == object.end()) {
        if (required) {
            error = std::string("missing required field '") + key + "'";
            return false;
        }
        return true;
    }
    if (!it->is_string()) {
        error = std::string("field '") + key + "' must be a string";
        return false;
    }
    out = it->get<std::string>();
    return true;
}

} // namespace

bool is_valid_mod_id(const std::string& id) {
    if (id.empty() || id.size() > 128) {
        return false;
    }
    for (size_t i = 0; i < id.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(id[i]);
        if (std::islower(c) || std::isdigit(c) || c == '.' || c == '_' || c == '-') {
            continue;
        }
        return false;
    }
    return std::islower(static_cast<unsigned char>(id[0])) ||
           std::isdigit(static_cast<unsigned char>(id[0]));
}

bool parse_manifest(const std::string& text, Manifest& out, std::string& error) {
    Json root;
    try {
        root = Json::parse(text);
    } catch (const std::exception& exception) {
        error = std::string("invalid JSON: ") + exception.what();
        return false;
    }

    if (!root.is_object()) {
        error = "manifest root must be a JSON object";
        return false;
    }

    Manifest manifest;

    if (root.contains("schema")) {
        if (!root["schema"].is_number_integer()) {
            error = "field 'schema' must be an integer";
            return false;
        }
        manifest.schema = root["schema"].get<int>();
        if (manifest.schema != 1) {
            error = "unsupported manifest schema " + std::to_string(manifest.schema);
            return false;
        }
    }

    if (!require_string(root, "id", true, manifest.id, error)) {
        return false;
    }
    if (!is_valid_mod_id(manifest.id)) {
        error = "id must match [a-z0-9][a-z0-9._-]{0,127}";
        return false;
    }

    if (!require_string(root, "version", true, manifest.version, error)) {
        return false;
    }

    if (!require_string(root, "name", false, manifest.name, error)) {
        return false;
    }
    if (!require_string(root, "entry", false, manifest.entry, error)) {
        return false;
    }
    if (!require_string(root, "shutdown", false, manifest.shutdown, error)) {
        return false;
    }

    manifest.entry = manifest.entry.empty() ? MOCHA_DEFAULT_ENTRY : manifest.entry;
    manifest.shutdown = manifest.shutdown.empty() ? MOCHA_DEFAULT_SHUTDOWN : manifest.shutdown;

    if (root.contains("abi")) {
        if (!root["abi"].is_number_integer()) {
            error = "field 'abi' must be an integer";
            return false;
        }
        manifest.abi = root["abi"].get<int>();
    }
    if (manifest.abi != static_cast<int>(MOCHA_ABI_VERSION)) {
        error = "mod ABI " + std::to_string(manifest.abi) + " does not match loader ABI " +
                std::to_string(MOCHA_ABI_VERSION);
        return false;
    }

    if (root.contains("dependencies")) {
        const auto& dependencies = root["dependencies"];
        if (!dependencies.is_array()) {
            error = "field 'dependencies' must be an array";
            return false;
        }
        for (const auto& item : dependencies) {
            if (!item.is_object()) {
                error = "each dependency must be an object";
                return false;
            }
            Dependency dependency;
            if (!require_string(item, "id", true, dependency.id, error)) {
                return false;
            }
            if (!is_valid_mod_id(dependency.id)) {
                error = "dependency id '" + dependency.id + "' is invalid";
                return false;
            }
            if (!require_string(item, "version", false, dependency.constraint, error)) {
                return false;
            }
            if (dependency.constraint.empty()) {
                dependency.constraint = "*";
            }
            if (item.contains("optional")) {
                if (!item["optional"].is_boolean()) {
                    error = "dependency field 'optional' must be a boolean";
                    return false;
                }
                dependency.optional = item["optional"].get<bool>();
            }
            manifest.dependencies.push_back(std::move(dependency));
        }
    }

    if (manifest.name.empty()) {
        manifest.name = manifest.id;
    }

    out = std::move(manifest);
    return true;
}

} // namespace mocha
