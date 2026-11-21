#ifndef MOCHA_PLAN_H
#define MOCHA_PLAN_H

#include "manifest.h"

#include <string>
#include <vector>

namespace mocha {

struct PlannedMod {
    Manifest manifest;
    bool enabled = false;
    std::string reason;
};

struct ResolveResult {
    std::vector<PlannedMod> mods;
    std::vector<Manifest> load_order;
    std::vector<std::string> errors;
};

ResolveResult resolve_mods(std::vector<Manifest> manifests);

} // namespace mocha

#endif
