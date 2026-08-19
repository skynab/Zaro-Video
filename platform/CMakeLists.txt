add_subdirectory(qrhi)
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
target_compile_options(zaro_platform_ffmpeg PRIVATE
    -Wno-old-style-cast
    -Wno-sign-conversion
)

if(APPLE)
    target_link_libraries(zaro_platform_ffmpeg PRIVATE
        "-framework VideoToolbox"
        "-framework CoreMedia"
        "-framework CoreVideo"
    )
endif()
