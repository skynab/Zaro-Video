# Prefer a system Catch2 (brew install catch2) and fall back to fetching it, so
# a clean checkout builds on a machine with nothing preinstalled.
find_package(Catch2 3 QUIET)

if(NOT Catch2_FOUND)
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
endif()

include(Catch)

add_executable(zaro_core_tests
    time/test_rational.cpp
    time/test_rational_time.cpp
    time/test_time_range.cpp
    time/test_timecode.cpp
)

target_link_libraries(zaro_core_tests
    PRIVATE
        zaro::core
        zaro::warnings
        Catch2::Catch2WithMain
)

catch_discover_tests(zaro_core_tests)
