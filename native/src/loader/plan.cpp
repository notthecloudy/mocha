#include "plan.h"

#include "semver.h"

#include <algorithm>
#include <map>
#include <set>

namespace mocha {
namespace {

bool is_enabled(const std::map<std::string, size_t>& index,
                const std::vector<PlannedMod>& mods, const std::string& id) {
    const auto it = index.find(id);
    return it != index.end() && mods[it->second].enabled;
}

void disable(PlannedMod& mod, const std::string& reason) {
    if (mod.enabled) {
        mod.enabled = false;
        mod.reason = reason;
    }
}

} // namespace

ResolveResult resolve_mods(std::vector<Manifest> manifests) {
    ResolveResult result;

    std::sort(manifests.begin(), manifests.end(), [](const Manifest& a, const Manifest& b) {
        if (a.id != b.id) {
            return a.id < b.id;
        }
        return a.manifest_path < b.manifest_path;
    });

    for (auto& manifest : manifests) {
        PlannedMod planned;
        planned.manifest = std::move(manifest);

        Version version;
        if (!parse_version(planned.manifest.version, version)) {
            planned.reason = "invalid version '" + planned.manifest.version + "'";
        } else {
            planned.enabled = true;
        }
        result.mods.push_back(std::move(planned));
    }

    std::map<std::string, size_t> index;
    std::map<std::string, size_t> counts;
    for (size_t i = 0; i < result.mods.size(); ++i) {
        ++counts[result.mods[i].manifest.id];
        index[result.mods[i].manifest.id] = i;
    }
    for (auto& mod : result.mods) {
        if (counts[mod.manifest.id] > 1) {
            disable(mod, "duplicate mod id");
        }
    }

    bool changed = true;
    size_t guard = 0;
    while (changed && guard++ <= result.mods.size() + 2) {
        changed = false;

        for (auto& mod : result.mods) {
            if (!mod.enabled) {
                continue;
            }
            Version version;
            parse_version(mod.manifest.version, version);

            for (const auto& dependency : mod.manifest.dependencies) {
                const auto it = index.find(dependency.id);
                if (it == index.end()) {
                    if (!dependency.optional) {
                        disable(mod, "missing dependency '" + dependency.id + "'");
                        changed = true;
                        break;
                    }
                    continue;
                }

                const PlannedMod& target = result.mods[it->second];
                if (!target.enabled) {
                    if (!dependency.optional) {
                        disable(mod, "dependency '" + dependency.id + "' could not be loaded");
                        changed = true;
                        break;
                    }
                    continue;
                }

                Version target_version;
                parse_version(target.manifest.version, target_version);
                std::string error;
                if (!satisfies(target_version, dependency.constraint, &error)) {
                    disable(mod, "dependency '" + dependency.id + "' version " +
                                     target.manifest.version + " does not satisfy '" +
                                     dependency.constraint + "'");
                    changed = true;
                    break;
                }
            }
        }
    }

    std::vector<std::string> order;
    bool cycle = false;

    for (size_t pass = 0; pass <= result.mods.size() + 1; ++pass) {
        order.clear();
        cycle = false;

        std::set<std::string> pending;
        for (const auto& mod : result.mods) {
            if (mod.enabled) {
                pending.insert(mod.manifest.id);
            }
        }

        std::map<std::string, std::set<std::string>> edges;
        std::map<std::string, int> indegree;
        for (const auto& id : pending) {
            indegree[id] = 0;
        }
        for (const auto& id : pending) {
            const PlannedMod& mod = result.mods[index[id]];
            for (const auto& dependency : mod.manifest.dependencies) {
                if (pending.find(dependency.id) == pending.end()) {
                    continue;
                }
                if (edges[dependency.id].insert(id).second) {
                    ++indegree[id];
                }
            }
        }

        while (!pending.empty()) {
            std::string next;
            for (const auto& id : pending) {
                if (indegree[id] == 0) {
                    next = id;
                    break;
                }
            }
            if (next.empty()) {
                cycle = true;
                break;
            }

            pending.erase(next);
            order.push_back(next);
            for (const auto& dependent : edges[next]) {
                --indegree[dependent];
            }
        }

        if (!cycle) {
            break;
        }

        bool disabled_any = false;
        for (auto& mod : result.mods) {
            if (mod.enabled && pending.count(mod.manifest.id) > 0) {
                disable(mod, "dependency cycle");
                disabled_any = true;
            }
        }
        for (auto& mod : result.mods) {
            if (!mod.enabled) {
                continue;
            }
            for (const auto& dependency : mod.manifest.dependencies) {
                if (!dependency.optional && !is_enabled(index, result.mods, dependency.id)) {
                    disable(mod, "dependency '" + dependency.id + "' could not be loaded");
                    disabled_any = true;
                    break;
                }
            }
        }
        if (!disabled_any) {
            break;
        }
    }

    for (const auto& mod : result.mods) {
        if (!mod.enabled) {
            result.errors.push_back(mod.manifest.id + ": " + mod.reason);
        }
    }

    for (const auto& id : order) {
        const auto it = index.find(id);
        if (it != index.end() && result.mods[it->second].enabled) {
            result.load_order.push_back(result.mods[it->second].manifest);
        }
    }

    return result;
}

} // namespace mocha
