#include "mocha_example_util.h"

#include <mocha/mocha_api.h>

MOCHA_MOD_EXPORT int mocha_mod_init(const MochaApi* mocha) {
    mocha_example::write_result(mocha, "missing-dep.result", "called=true");
    mocha_example::log(mocha, MOCHA_LOG_ERROR,
                       "missing-dep: init ran even though a dependency is absent");
    return 0;
}
