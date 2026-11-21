#include "manifest.h"
#include "plan.h"
#include "semver.h"

#include <mocha/mocha_api.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(condition)) {                                                     \
            ++g_failures;                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);    \
        }                                                                       \
    } while (0)

using mocha::Dependency;using mocha::Manifest;
using mocha::ResolveResult;
using mocha::Version;

Manifest make_mod(const std::string& id, const std::string& version,
                  std::vector<Dependency> dependencies = {}) {
    Manifest manifest;
    manifest.id = id;
    manifest.version = version;
    manifest.name = id;
    manifest.manifest_path = id + ".json";
    manifest.library_path = id + ".so";
    manifest.dependencies = std::move(dependencies);
    return manifest;
}

Dependency dep(const std::string& id, const std::string& constraint = "*",
               bool optional = false) {
    Dependency dependency;
    dependency.id = id;
    dependency.constraint = constraint;
    dependency.optional = optional;
    return dependency;
}

const mocha::PlannedMod* find_planned(const ResolveResult& result, const std::string& id) {
    for (const auto& mod : result.mods) {
        if (mod.manifest.id == id) {
            return &mod;
        }
    }
    return nullptr;
}

bool order_has(const ResolveResult& result, const std::string& id) {
    for (const auto& manifest : result.load_order) {
        if (manifest.id == id) {
            return true;
        }
    }
    return false;
}

int order_index(const ResolveResult& result, const std::string& id) {
    for (size_t i = 0; i < result.load_order.size(); ++i) {
        if (result.load_order[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void test_semver() {
    Version version;
    CHECK(mocha::parse_version("1.2.3", version));
    CHECK(version.major == 1 && version.minor == 2 && version.patch == 3);
    CHECK(version.prerelease.empty());
    CHECK(mocha::parse_version("1.2", version));
    CHECK(version.major == 1 && version.minor == 2 && version.patch == 0);
    CHECK(mocha::parse_version("1.0.0-alpha.1+build.7", version));
    CHECK(version.prerelease == "alpha.1");
    CHECK(!mocha::parse_version("1.2.3.4", version));
    CHECK(!mocha::parse_version("", version));
    CHECK(!mocha::parse_version("v1.2.3", version));

    Version a;
    Version b;
    CHECK(mocha::parse_version("1.2.3", a));
    CHECK(mocha::parse_version("1.2.4", b));
    CHECK(mocha::compare_versions(a, b) < 0);
    CHECK(mocha::compare_versions(b, a) > 0);
    CHECK(mocha::compare_versions(a, a) == 0);

    CHECK(mocha::parse_version("1.0.0-alpha", a));
    CHECK(mocha::parse_version("1.0.0", b));
    CHECK(mocha::compare_versions(a, b) < 0);
    CHECK(mocha::parse_version("1.0.0-alpha.2", a));
    CHECK(mocha::parse_version("1.0.0-alpha.10", b));
    CHECK(mocha::compare_versions(a, b) < 0);

    CHECK(mocha::parse_version("1.4.2", a));
    CHECK(mocha::satisfies(a, "^1.2.0", nullptr));
    CHECK(mocha::satisfies(a, "~1.4.0", nullptr));
    CHECK(mocha::satisfies(a, ">=1.0.0 <2.0.0", nullptr));
    CHECK(mocha::satisfies(a, "1.x", nullptr));
    CHECK(mocha::satisfies(a, "1.4.2", nullptr));
    CHECK(mocha::satisfies(a, ">=1.0.0 || <0.5.0", nullptr));

    CHECK(mocha::parse_version("2.0.0", b));
    CHECK(!mocha::satisfies(b, "^1.2.0", nullptr));
    CHECK(!mocha::satisfies(b, "~1.4.0", nullptr));
    CHECK(!mocha::satisfies(b, ">=1.0.0 <2.0.0", nullptr));
    CHECK(!mocha::satisfies(b, "1.x", nullptr));
    CHECK(!mocha::satisfies(b, "=1.4.2", nullptr));

    std::string error;
    CHECK(!mocha::satisfies(b, "^1.2.0", &error));
    CHECK(!error.empty());
}

void test_manifest() {
    const char* valid = R"({
        "schema": 1,
        "id": "mocha.example.hello",
        "name": "Hello",
        "version": "1.2.0",
        "abi": 1,
        "dependencies": [
            {"id": "mocha.example.base", "version": ">=1.0.0"},
            {"id": "mocha.example.optional", "optional": true}
        ]
    })";

    Manifest manifest;
    std::string error;
    CHECK(mocha::parse_manifest(valid, manifest, error));
    CHECK(manifest.id == "mocha.example.hello");
    CHECK(manifest.name == "Hello");
    CHECK(manifest.entry == MOCHA_DEFAULT_ENTRY);
    CHECK(manifest.shutdown == MOCHA_DEFAULT_SHUTDOWN);
    CHECK(manifest.dependencies.size() == 2);
    CHECK(manifest.dependencies[0].id == "mocha.example.base");
    CHECK(manifest.dependencies[0].constraint == ">=1.0.0");
    CHECK(!manifest.dependencies[0].optional);
    CHECK(manifest.dependencies[1].constraint == "*");
    CHECK(manifest.dependencies[1].optional);

    CHECK(!mocha::parse_manifest("{", manifest, error));
    CHECK(!mocha::parse_manifest(R"({"schema": 2, "id": "a", "version": "1.0.0"})",
                                 manifest, error));
    CHECK(!mocha::parse_manifest(R"({"version": "1.0.0"})", manifest, error));
    CHECK(!mocha::parse_manifest(R"({"id": "Bad.Id", "version": "1.0.0"})", manifest, error));
    CHECK(!mocha::parse_manifest(R"({"id": "a", "version": "1.0.0", "abi": 2})",
                                 manifest, error));
    CHECK(!mocha::parse_manifest(
        R"({"id": "a", "version": "1.0.0", "dependencies": [{"version": "1.0.0"}]})",
        manifest, error));
    CHECK(mocha::is_valid_mod_id("a"));
    CHECK(mocha::is_valid_mod_id("0mod"));
    CHECK(mocha::is_valid_mod_id("my-mod_1.2"));
    CHECK(!mocha::is_valid_mod_id(""));
    CHECK(!mocha::is_valid_mod_id("-mod"));
    CHECK(!mocha::is_valid_mod_id("mod/x"));
}

void test_plan() {
    {
        ResolveResult result = mocha::resolve_mods({
            make_mod("a.dependent", "1.0.0", {dep("z.hello", ">=1.0.0")}),
            make_mod("z.hello", "1.2.0"),
        });
        CHECK(result.errors.empty());
        CHECK(result.load_order.size() == 2);
        CHECK(order_index(result, "z.hello") == 0);
        CHECK(order_index(result, "a.dependent") == 1);
    }

    {
        ResolveResult result = mocha::resolve_mods({
            make_mod("mocha.example.missing-dep", "1.0.0", {dep("mocha.example.absent")}),
        });
        const auto* planned = find_planned(result, "mocha.example.missing-dep");
        CHECK(planned != nullptr);
        CHECK(planned && !planned->enabled);
        CHECK(planned && planned->reason.find("missing dependency") != std::string::npos);
        CHECK(result.load_order.empty());
        CHECK(result.errors.size() == 1);
    }

    {
        ResolveResult result = mocha::resolve_mods({
            make_mod("a", "1.0.0", {dep("b")}),
            make_mod("b", "1.0.0", {dep("a")}),
        });
        CHECK(result.load_order.empty());
        const auto* a = find_planned(result, "a");
        const auto* b = find_planned(result, "b");
        CHECK(a && !a->enabled);
        CHECK(b && !b->enabled);
        CHECK(a && a->reason == "dependency cycle");
        CHECK(b && b->reason == "dependency cycle");
    }

    {
        ResolveResult result = mocha::resolve_mods({
            make_mod("a", "1.0.0", {dep("b", "^2.0.0")}),
            make_mod("b", "1.5.0"),
        });
        const auto* a = find_planned(result, "a");
        CHECK(a && !a->enabled);
        CHECK(order_has(result, "b"));
        CHECK(!order_has(result, "a"));
    }

    {
        ResolveResult result = mocha::resolve_mods({
            make_mod("a", "1.0.0", {dep("b", ">=1.0.0", true)}),
        });
        const auto* a = find_planned(result, "a");
        CHECK(a && a->enabled);
        CHECK(order_has(result, "a"));
        CHECK(result.errors.empty());
    }

    {
        ResolveResult result = mocha::resolve_mods({
            make_mod("a", "1.0.0"),
            make_mod("a", "1.1.0"),
        });
        CHECK(result.load_order.empty());
        CHECK(result.errors.size() == 2);
        CHECK(find_planned(result, "a") && !find_planned(result, "a")->enabled);
    }

    {
        ResolveResult result = mocha::resolve_mods({
            make_mod("a", "not-a-version"),
        });
        const auto* a = find_planned(result, "a");
        CHECK(a && !a->enabled);
        CHECK(a && a->reason.find("invalid version") != std::string::npos);
    }

    {
        ResolveResult result = mocha::resolve_mods({
            make_mod("a", "1.0.0", {dep("c")}),
            make_mod("b", "1.0.0", {dep("a")}),
            make_mod("c", "1.0.0"),
        });
        CHECK(result.load_order.size() == 3);
        CHECK(order_index(result, "c") < order_index(result, "a"));
        CHECK(order_index(result, "a") < order_index(result, "b"));
    }
}

} // namespace

int main() {
    test_semver();
    test_manifest();
    test_plan();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
