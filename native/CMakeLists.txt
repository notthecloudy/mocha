add_library(mocha SHARED
    src/loader/loader.cpp
    src/loader/log.cpp
    src/loader/paths.cpp
    src/loader/runtime_gate.cpp
    src/loader/il2cpp_api.cpp
    src/loader/semver.cpp
    src/loader/manifest.cpp
    src/loader/plan.cpp
)

target_include_directories(mocha PRIVATE
    "${MOCHA_SDK_INCLUDE}"
    "${MOCHA_THIRD_PARTY}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_options(mocha PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -fvisibility=hidden
)

target_link_libraries(mocha PRIVATE log dl)

target_link_options(mocha PRIVATE
    "-Wl,-z,max-page-size=16384"
    "-Wl,-z,common-page-size=16384"
    "-Wl,-z,now"
    "-Wl,-z,relro"
)

add_library(mocha_shim SHARED src/shim/libmain_shim.cpp)
set_target_properties(mocha_shim PROPERTIES OUTPUT_NAME main)
target_compile_options(mocha_shim PRIVATE -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden)
target_link_libraries(mocha_shim PRIVATE log dl)
target_link_options(mocha_shim PRIVATE
    "-Wl,-z,max-page-size=16384"
    "-Wl,-z,common-page-size=16384"
    "-Wl,-z,now"
    "-Wl,-z,relro"
)
