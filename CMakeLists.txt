cmake_minimum_required(VERSION 3.24)

project(CutReel
    VERSION 0.7.0
    DESCRIPTION "A non-linear video editor"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The sources are UTF-8, and MSVC has to be told so.
#
# Every other compiler here assumes it. cl assumes the machine's ANSI code page
# instead, so the em dashes and the middle dots that the panels put on screen --
# "EQ - off", "1920 x 1080", "-inf" -- come out of a narrow literal as two
# mojibake characters rather than one, and every file carrying one draws a
# C4819. Set globally rather than through zaro::warnings: what a byte in a
# string literal means is not a warning policy, and it has to be the same answer
# for the third-party code compiled in this tree as for ours.
if(MSVC)
    add_compile_options(/utf-8)
endif()

if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

option(ZARO_BUILD_TESTS       "Build the test suite"           ON)
option(ZARO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(ZARO_SANITIZE          "Enable ASan+UBSan (Debug only)"  OFF)

include(CompilerWarnings)

# The version, as a header, generated from project(VERSION) above so that the
# number lives in exactly one place. Anything that wants to show it links
# zaro::version and includes <zaro/Version.h>.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.h.in"
    "${CMAKE_BINARY_DIR}/generated/include/zaro/Version.h"
    @ONLY)
add_library(zaro_version INTERFACE)
add_library(zaro::version ALIAS zaro_version)
target_include_directories(zaro_version
    INTERFACE $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>)

add_subdirectory(core)
add_subdirectory(ui-core)
add_subdirectory(platform)
add_subdirectory(tools)
add_subdirectory(app)

if(ZARO_BUILD_TESTS)
    include(CTest)
    enable_testing()

    # Catch2 is fetched and built as part of this project rather than found
    # preinstalled, and it is done here rather than in core/tests. Both halves
    # of that were bugs on Windows:
    #
    # * A find_package config file creates *directory-scoped* imported targets.
    #   Found from core/tests, Catch2::Catch2WithMain existed in core/tests and
    #   nowhere else, and the sibling suites failed to configure with "the
    #   target was not found".
    # * Catch2's own build asks for cxx_std_14, so a prebuilt copy -- vcpkg's,
    #   Homebrew's, the distribution's -- has a library compiled without the
    #   C++17 string_view stringifiers, while our C++20 test sources declare
    #   and call them. That link fails on the one assertion that prints a
    #   string_view, with an undefined Catch::StringMaker<string_view>.
    #
    # Both stayed hidden for as long as no machine had a Catch2 to find: macOS
    # and Linux fetched one, which is a real target, global, and built to our
    # standard. Only Windows, where vcpkg supplies one, took the other path.
    # Fetching everywhere means every platform builds the same Catch2 the same
    # way. A machine without network can still point at a local checkout with
    # -DFETCHCONTENT_SOURCE_DIR_CATCH2=<path>.
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.15.3
        GIT_SHALLOW    TRUE
    )
    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    # Brings in catch_discover_tests, which every suite below registers with.
    include(Catch)

    add_subdirectory(core/tests)
    add_subdirectory(platform/tests)
    add_subdirectory(app/tests)
endif()

include(Packaging)

message(STATUS "")
message(STATUS "CutReel ${PROJECT_VERSION}")
message(STATUS "  build type ......... ${CMAKE_BUILD_TYPE}")
message(STATUS "  tests .............. ${ZARO_BUILD_TESTS}")
message(STATUS "  warnings as errors . ${ZARO_WARNINGS_AS_ERRORS}")
message(STATUS "  sanitizers ......... ${ZARO_SANITIZE}")
message(STATUS "")
