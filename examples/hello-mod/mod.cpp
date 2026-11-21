#include "mocha_example_util.h"

#include <mocha/mocha_api.h>

#include <cstdint>
#include <string>

namespace {

constexpr const char* kAssembly = "Assembly-CSharp.dll";
constexpr const char* kNamespace = "MochaTestApp";
constexpr const char* kClass = "MochaTestTarget";

int run(const MochaApi* mocha) {
    using namespace mocha_example;

    void* klass = find_class(mocha, kAssembly, kNamespace, kClass);
    if (!klass) {
        log(mocha, MOCHA_LOG_ERROR, "hello: MochaTestTarget not found");
        return 1;
    }

    const void* add = find_method(mocha, klass, "Add", 2);
    const void* echo = find_method(mocha, klass, "Echo", 1);
    const void* increment = find_method(mocha, klass, "Increment", 0);
    const void* info = find_method(mocha, klass, "Info", 0);
    if (!add || !echo || !increment || !info) {
        log(mocha, MOCHA_LOG_ERROR, "hello: expected test methods are missing");
        return 1;
    }

    int32_t left = 2;
    int32_t right = 3;
    void* add_params[] = {&left, &right};
    int32_t sum = 0;
    if (!invoke_int(mocha, add, add_params, sum)) {
        log(mocha, MOCHA_LOG_ERROR, "hello: Add invocation failed");
        return 1;
    }

    void* argument = mocha->il2cpp->string_new("mocha");
    void* echo_params[] = {argument};
    std::string echoed;
    if (!invoke_string(mocha, echo, echo_params, echoed)) {
        log(mocha, MOCHA_LOG_ERROR, "hello: Echo invocation failed");
        return 1;
    }

    int32_t counter_before = -1;
    get_static_int(mocha, klass, "Counter", counter_before);

    void* exception = nullptr;
    mocha->il2cpp->runtime_invoke(increment, nullptr, nullptr, &exception);

    int32_t counter_after = -1;
    get_static_int(mocha, klass, "Counter", counter_after);

    std::string info_text;
    invoke_string(mocha, info, nullptr, info_text);

    const bool ok = sum == 5 && echoed == "echo:mocha" && counter_before == 0 &&
                    counter_after == 1 && exception == nullptr && !info_text.empty();
    const std::string result = std::string("ok=") + (ok ? "true" : "false") +
                               "\nadd=2+3=" + std::to_string(sum) + "\necho=" + echoed +
                               "\ncounter_before=" + std::to_string(counter_before) +
                               "\ncounter_after=" + std::to_string(counter_after) +
                               "\ninfo=" + info_text;
    write_result(mocha, "hello.result", result);
    log(mocha, ok ? MOCHA_LOG_INFO : MOCHA_LOG_ERROR, "hello: " + result);
    return ok ? 0 : 1;
}

} // namespace

MOCHA_MOD_EXPORT int mocha_mod_init(const MochaApi* mocha) {
    return run(mocha);
}
