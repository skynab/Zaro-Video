add_executable(zaro_media_tests
    test_probe.cpp
    test_video_decoder.cpp
    test_audio_decoder.cpp
    test_frame_types.cpp
)

target_link_libraries(zaro_media_tests
    PRIVATE
        zaro::platform_ffmpeg
        zaro::warnings
        Catch2::Catch2WithMain
)

# Tests locate the generated fixtures through this, so they can run from any
# working directory -- including from ctest, which does not promise one.
target_compile_definitions(zaro_media_tests PRIVATE
    ZARO_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata/media"
)

catch_discover_tests(zaro_media_tests)
