find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.12.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(zaro_core
    src/Check.cpp
    src/Error.cpp
    src/edit/Command.cpp
    src/edit/CommandStack.cpp
    src/edit/Operations.cpp
    src/edit/Snapping.cpp
    src/edit/Sync.cpp
    src/io/CubeLut.cpp
    src/io/OtioIo.cpp
    src/io/SubtitleIo.cpp
    src/io/ProjectIo.cpp
    src/playback/AudioRingBuffer.cpp
    src/playback/PlaybackScheduler.cpp
    src/playback/Transport.cpp
    src/render/AudioGraph.cpp
    src/render/AudioProcessor.cpp
    src/render/Loudness.cpp
    src/render/BakeLut.cpp
    src/render/ColorPipeline.cpp
    src/render/Ducking.cpp
    src/render/EffectStack.cpp
    src/render/FrameCache.cpp
    src/render/RenderCache.cpp
    src/render/Compare.cpp
    src/render/Compositing.cpp
    src/render/CurveTable.cpp
    src/render/Grade.cpp
    src/render/Keyer.cpp
    src/render/LutTable.cpp
    src/render/PathRaster.cpp
    src/model/MediaSearch.cpp
    src/io/MediaBrowser.cpp
    src/io/ProjectLock.cpp
    src/io/Relink.cpp
    src/io/ReviewNotes.cpp
    src/render/Reframe.cpp
    src/render/Remix.cpp
    src/render/Resample.cpp
    src/render/SmartRender.cpp
    src/render/Stabilise.cpp
    src/render/Tracker.cpp
    src/render/Qualifier.cpp
    src/render/ShapeRaster.cpp
    src/render/TextRasterizer.cpp
    src/render/RenderGraph.cpp
    src/render/SceneDetect.cpp
    src/render/Scopes.cpp
    src/render/ShotMatch.cpp
    src/render/ToneMap.cpp
    src/render/TransitionShape.cpp
    src/render/RgbaImage.cpp
    src/model/Animation.cpp
    src/model/Caption.cpp
    src/model/AudioRole.cpp
    src/model/Clip.cpp
    src/model/Effect.cpp
    src/model/Graphic.cpp
    src/model/Mask.cpp
    src/model/ClipEffects.cpp
    src/model/Project.cpp
    src/model/Sequence.cpp
    src/model/ToneCurve.cpp
    src/model/Track.cpp
    src/model/Transition.cpp
    src/media/AudioAlign.cpp
    src/media/AudioBuffer.cpp
    src/media/ColorInfo.cpp
    src/media/PixelFormat.cpp
    src/media/VideoFrame.cpp
    src/media/Waveform.cpp
    src/time/Rational.cpp
    src/time/RationalTime.cpp
    src/time/TimeRange.cpp
    src/time/Timecode.cpp
)
add_library(zaro::core ALIAS zaro_core)

target_include_directories(zaro_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# nlohmann_json is PRIVATE on purpose: the JSON library is an implementation
# detail of core/io and must not become part of every consumer's include path.
target_link_libraries(zaro_core
    PRIVATE zaro::warnings nlohmann_json::nlohmann_json)

target_compile_features(zaro_core PUBLIC cxx_std_20)

set_target_properties(zaro_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# core/ deliberately links nothing from Qt or FFmpeg. Everything here must stay
# runnable headless so the edit engine can be tested, scripted and rendered
# without a window server.
#
# core/media therefore holds only interfaces and value types -- VideoFrame,
# AudioBuffer, MediaInfo, the decoder interfaces. The libav* implementation of
# those interfaces lives in platform/ffmpeg and is the only place in the
# codebase that includes an FFmpeg header.
