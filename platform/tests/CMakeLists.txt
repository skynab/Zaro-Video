find_package(Qt6 REQUIRED COMPONENTS Gui)

add_executable(zaro_media_tests
    test_gpu_compositor.cpp
    test_probe.cpp
    test_video_decoder.cpp
    test_audio_decoder.cpp
    test_frame_types.cpp
)

target_link_libraries(zaro_media_tests
    PRIVATE
        zaro::platform_ffmpeg
        zaro::platform_qrhi
        Qt6::Gui
        zaro::warnings
        Catch2::Catch2WithMain
)

# Tests locate the generated fixtures through this, so they can run from any
# working directory -- including from ctest, which does not promise one.
target_include_directories(zaro_media_tests PRIVATE ${CMAKE_SOURCE_DIR}/core/tests)

target_compile_definitions(zaro_media_tests PRIVATE
    ZARO_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata/media"
)

# The GPU tests need a platform plugin even though they never open a window.
# Setting it here means ctest works without the caller knowing that.
catch_discover_tests(zaro_media_tests
    TEST_SPEC "~[.]"
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
