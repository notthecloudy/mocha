cmake_minimum_required(VERSION 3.22)
project(mocha LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(MOCHA_SDK_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include")
set(MOCHA_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/native/third_party")

if(ANDROID)
    add_subdirectory(native)
    add_subdirectory(examples)
else()
    enable_testing()
    add_subdirectory(tests/unit)
endif()
