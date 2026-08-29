add_subdirectory(qrhi)
add_subdirectory(qtext)
add_subdirectory(sdl)

find_package(FFmpeg REQUIRED)

add_library(zaro_platform_ffmpeg
    ffmpeg/src/Ffi.cpp
    ffmpeg/src/Probe.cpp
    ffmpeg/src/VideoDecoder.cpp
    ffmpeg/src/AudioDecoder.cpp
    ffmpeg/src/ImageIo.cpp
    ffmpeg/src/ProjectMediaSource.cpp
    ffmpeg/src/Encoder.cpp
    ffmpeg/src/WaveformStore.cpp
    ffmpeg/src/MakeProxy.cpp
    ffmpeg/src/SmartRender.cpp
    ffmpeg/src/RenderSequence.cpp
)
add_library(zaro::platform_ffmpeg ALIAS zaro_platform_ffmpeg)

target_include_directories(zaro_platform_ffmpeg
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg/src
)

target_link_libraries(zaro_platform_ffmpeg
    PUBLIC  zaro::core
    PRIVATE zaro::warnings FFmpeg::FFmpeg
)

# FFmpeg's public headers are C: several of its macros expand to C-style casts,
# and its integer types do not line up with ours without narrowing. Relaxing
# these two warnings at the boundary is the price of using a C library; they
# stay on everywhere else.
#
# These are GCC and Clang spellings, and MSVC is not offered them: cl reads
# `-Wno-x` as `/W` with a bad numeric argument and stops with D8021 rather
# than ignoring what it does not know. It does not need them either -- the
# third-party headers reach it as /external: includes at /external:W0.
if(NOT MSVC)
    target_compile_options(zaro_platform_ffmpeg PRIVATE
        -Wno-old-style-cast
        -Wno-sign-conversion
    )
endif()

if(APPLE)
    target_link_libraries(zaro_platform_ffmpeg PRIVATE
        "-framework VideoToolbox"
        "-framework CoreMedia"
        "-framework CoreVideo"
    )
endif()
