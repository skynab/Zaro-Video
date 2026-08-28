find_package(Qt6 REQUIRED COMPONENTS Widgets)
# The GUI tests drive a real window against a real GPU, so they need Widgets
# and the panels the window is built from -- the same sources zaro-preview
# links, less its main().
add_executable(zaro_app_tests
    GuiFixture.cpp
    GuiFixture.h
    # Listed in the order the sections ran when this suite was a block inside
    # main(). Catch2 runs tests in registration order, and while the Rewind
    # guard means a test no longer depends on what ran before it, the two that
    # add a track or a sequence still reallocate vectors the fixture holds
    # references into -- so they stay last, where they were written to be.
    test_timeline_editing.cpp
    test_effects.cpp
    test_grading.cpp
    test_masks.cpp
    test_time_and_transitions.cpp
    test_audio.cpp
    test_playback.cpp
    test_titles_and_captions.cpp
    test_media_management.cpp
    test_project_lifecycle.cpp
)

target_link_libraries(zaro_app_tests
    PRIVATE
        zaro_app_ui
        zaro::warnings
        Qt6::Widgets
        Catch2::Catch2WithMain
)

target_include_directories(zaro_app_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_compile_definitions(zaro_app_tests PRIVATE
    ZARO_SCRATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    ZARO_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata/media"
)

# Qt's headers trip warnings that are right for our code and wrong for a
# framework this size. Same set the application target carries.
target_compile_options(zaro_app_tests PRIVATE
    -Wno-old-style-cast
    -Wno-sign-conversion
    -Wno-conversion
    -Wno-shadow
)

set_target_properties(zaro_app_tests PROPERTIES AUTOMOC ON)

# Registered as one test rather than through catch_discover_tests, which runs
# each TEST_CASE in its own process. Building the window costs a GPU context, a
# decode and a waveform pass; paying that sixty times over would take the suite
# from a minute to the better part of an hour, and the whole point of the shared
# fixture is that it is paid once.
#
# No QT_QPA_PLATFORM=offscreen here, unlike the media tests: that plugin has no
# QRhi, and a QRhiWidget on it never draws. These need a real platform, which is
# why CI runs them under a virtual display rather than headless.
add_test(NAME zaro_app_tests COMMAND zaro_app_tests)
set_tests_properties(zaro_app_tests PROPERTIES TIMEOUT 1800)
