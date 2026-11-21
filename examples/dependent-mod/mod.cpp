#include "mocha_example_util.h"

#include <mocha/mocha_api.h>

#include <cstdint>
#include <string>

namespace {

int run(const MochaApi* mocha) {
    using namespace mocha_example;

    void* klass = find_class(mocha, "Assembly-CSharp.dll", "MochaTestApp", "MochaTestTarget");
    if (!klass) {
        log(mocha, MOCHA_LOG_ERROR, "dependent: MochaTestTarget not found");
        return 1;
    }

    const void* add = find_method(mocha, klass, "Add", 2);
    if (!add) {
        log(mocha, MOCHA_LOG_ERROR, "dependent: Add not found");
        return 1;
    }

    int32_t left = 3;
    int32_t right = 4;
    void* params[] = {&left, &right};
    int32_t sum = 0;
    if (!invoke_int(mocha, add, params, sum)) {
        log(mocha, MOCHA_LOG_ERROR, "dependent: Add invocation failed");
        return 1;
    }

    const bool ok = sum == 7;
    write_result(mocha, "dependent.result",
                 std::string("ok=") + (ok ? "true" : "false") + "\nadd=3+4=" +
                     std::to_string(sum));
    log(mocha, ok ? MOCHA_LOG_INFO : MOCHA_LOG_ERROR,
        "dependent: add=3+4=" + std::to_string(sum));
    return ok ? 0 : 1;
}

} // namespace

MOCHA_MOD_EXPORT int mocha_mod_init(const MochaApi* mocha) {
    return run(mocha);
}
