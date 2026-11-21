#ifndef MOCHA_SEMVER_H
#define MOCHA_SEMVER_H

#include <string>
#include <string_view>

namespace mocha {

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string prerelease;
    bool valid = false;
};

bool parse_version(std::string_view text, Version& out);
int compare_versions(const Version& a, const Version& b);
bool satisfies(const Version& version, std::string_view constraint, std::string* error);

} // namespace mocha

#endif
