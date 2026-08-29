# Catch2 and catch_discover_tests come from the top-level CMakeLists.

add_executable(zaro_core_tests
    time/test_int128.cpp
    time/test_rational.cpp
    time/test_rational_time.cpp
    time/test_time_range.cpp
    time/test_timecode.cpp
    model/test_animation.cpp
    model/test_media_search.cpp
    model/test_pinning.cpp
    model/test_responsive.cpp
    model/test_track.cpp
    edit/test_delete_spans.cpp
    edit/test_command_stack.cpp
    edit/test_multicam.cpp
    edit/test_source_links.cpp
    edit/test_sync.cpp
    edit/test_time_remap.cpp
    edit/test_nesting.cpp
    edit/test_operations.cpp
    edit/test_snapping.cpp
    edit/test_fuzz.cpp
    io/test_relink.cpp
    io/test_review_notes.cpp
    io/test_media_browser.cpp
    io/test_environment.cpp
    io/test_project_lock.cpp
    io/test_versions.cpp
    io/test_templates.cpp
    io/test_cube_lut.cpp
    io/test_otio.cpp
    io/test_premiere.cpp
    io/test_xml.cpp
    io/test_subtitles.cpp
    io/test_project_io.cpp
    io/test_saving.cpp
    io/test_proxies.cpp
    media/test_waveform.cpp
    ui/test_keymap.cpp
    ui/test_timeline_layout.cpp
    playback/test_transport.cpp
    playback/test_ring_buffer.cpp
    playback/test_scheduler.cpp
    render/test_color_pipeline.cpp
    render/test_frame_cache.cpp
    render/test_compositing.cpp
    render/test_bake_lut.cpp
    render/test_compare.cpp
    render/test_ducking.cpp
    render/test_effects.cpp
    render/test_keyer.cpp
    render/test_path_raster.cpp
    render/test_reframe.cpp
    render/test_remix.cpp
    render/test_smart_render.cpp
    render/test_stabilise.cpp
    render/test_tracker.cpp
    render/test_render_cache.cpp
    render/test_scene_detect.cpp
    render/test_shot_match.cpp
    render/test_transitions.cpp
    render/test_vignette.cpp
    render/test_wheels.cpp
    render/test_render_graph.cpp
    render/test_curves.cpp
    render/test_grade.cpp
    render/test_qualifier.cpp
    render/test_scopes.cpp
    render/test_shapes.cpp
    render/test_text.cpp
    render/test_audio_graph.cpp
    render/test_audio_processing.cpp
    render/test_loudness.cpp
)

target_link_libraries(zaro_core_tests
    PRIVATE
        zaro::core
        zaro::ui_core
        zaro::warnings
        Catch2::Catch2WithMain
)

# ModelFixtures.h lives alongside the tests rather than in the library.
#
# core/src is on the path too, for the one private header worth testing
# directly: the XML reader behind the Premiere interchange. It is a parser
# written by hand, so its edges -- entities, CDATA, a truncated document -- are
# exactly what has to be pinned down, and reaching them only through a timeline
# format would test them by implication.
target_include_directories(zaro_core_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/render
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/io)

# Somewhere for the serialization tests to write a file.
target_compile_definitions(zaro_core_tests PRIVATE
    ZARO_SCRATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}")

# "~[.]" excludes Catch2's hidden tests -- the benchmarks. They are meant to be
# run deliberately: the 1080p ones take minutes under a sanitizer, which is long
# enough to look like a hang in CI.
catch_discover_tests(zaro_core_tests TEST_SPEC "~[.]")
