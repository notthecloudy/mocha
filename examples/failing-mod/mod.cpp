#include "mocha_example_util.h"

#include <mocha/mocha_api.h>

#include <string>

MOCHA_MOD_EXPORT int mocha_mod_init(const MochaApi* mocha) {
    mocha_example::write_result(mocha, "failing.result", "called=true\nreturn=7");
    mocha_example::log(mocha, MOCHA_LOG_INFO, "failing: intentionally returning 7");
    return 7;
}
