# Locates the libav* libraries and exposes one imported target, FFmpeg::FFmpeg,
# carrying all of them.
#
# On macOS these come from Homebrew, on Linux from the distribution, and on
# Windows they will come from the vcpkg manifest. Callers should not care which.
find_package(PkgConfig REQUIRED)

# FFmpeg 5.0 and later. The channel-layout API (AVChannelLayout) landed in 5.1
# and the display-matrix accessor changed again in 7.0, which Probe.cpp guards
# for; anything older than this needs more than a guard.
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec>=59
    libavformat>=59
    libavutil>=57
    libswscale>=6
    libswresample>=4
)

if(TARGET PkgConfig::FFMPEG AND NOT TARGET FFmpeg::FFmpeg)
    add_library(FFmpeg::FFmpeg INTERFACE IMPORTED)
    target_link_libraries(FFmpeg::FFmpeg INTERFACE PkgConfig::FFMPEG)
endif()
