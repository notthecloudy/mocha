add_executable(mocha_unit_tests
    test_main.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../../native/src/loader/semver.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../native/src/loader/manifest.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../native/src/loader/plan.cpp"
)

target_include_directories(mocha_unit_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../native/src/loader"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../native/third_party"
)

target_compile_options(mocha_unit_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME mocha_unit_tests COMMAND mocha_unit_tests)
