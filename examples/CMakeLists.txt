add_library(mocha_example_util INTERFACE)
target_include_directories(mocha_example_util INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${MOCHA_SDK_INCLUDE}"
)

function(mocha_add_example name)
    add_library(${name} SHARED "${name}/mod.cpp")
    set_target_properties(${name} PROPERTIES PREFIX "")
    target_link_libraries(${name} PRIVATE mocha_example_util)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden)
    target_link_options(${name} PRIVATE
        "-Wl,-z,max-page-size=16384"
        "-Wl,-z,common-page-size=16384"
        "-Wl,-z,now"
        "-Wl,-z,relro"
    )
endfunction()

mocha_add_example(hello-mod)
mocha_add_example(failing-mod)
mocha_add_example(dependent-mod)
mocha_add_example(missing-dep-mod)
