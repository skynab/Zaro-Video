cmake_minimum_required(VERSION 3.24)

project(ZaroVideo
    VERSION 0.1.0
    DESCRIPTION "A non-linear video editor"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

option(ZARO_BUILD_TESTS       "Build the test suite"           ON)
option(ZARO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(ZARO_SANITIZE          "Enable ASan+UBSan (Debug only)"  OFF)

include(CompilerWarnings)

add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(tools)
add_subdirectory(app)

if(ZARO_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(core/tests)
    add_subdirectory(platform/tests)
endif()

message(STATUS "")
message(STATUS "Zaro Video ${PROJECT_VERSION}")
message(STATUS "  build type ......... ${CMAKE_BUILD_TYPE}")
message(STATUS "  tests .............. ${ZARO_BUILD_TESTS}")
message(STATUS "  warnings as errors . ${ZARO_WARNINGS_AS_ERRORS}")
message(STATUS "  sanitizers ......... ${ZARO_SANITIZE}")
message(STATUS "")
