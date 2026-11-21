#ifndef MOCHA_MANIFEST_H
#define MOCHA_MANIFEST_H

#include <string>
#include <vector>

namespace mocha {

struct Dependency {
    std::string id;
    std::string constraint;
    bool optional = false;
};

struct Manifest {
    int schema = 1;
    std::string id;
    std::string name;
    std::string version;
    std::string entry;
    std::string shutdown;
    int abi = 1;
    std::vector<Dependency> dependencies;
    std::string library_path;
    std::string manifest_path;
};

bool parse_manifest(const std::string& text, Manifest& out, std::string& error);
bool is_valid_mod_id(const std::string& id);

} // namespace mocha

#endif
