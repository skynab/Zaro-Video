find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

add_library(zaro_platform_sdl src/AudioSink.cpp)
add_library(zaro::platform_sdl ALIAS zaro_platform_sdl)

target_include_directories(zaro_platform_sdl
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(zaro_platform_sdl
    PUBLIC  zaro::core
    PRIVATE zaro::warnings PkgConfig::SDL2
)
# SDL's headers are C and trip the same warnings FFmpeg's do.
#
# These are GCC and Clang spellings, and MSVC is not offered them: cl reads
# `-Wno-x` as `/W` with a bad numeric argument and stops with D8021 rather
# than ignoring what it does not know. It does not need them either -- the
# third-party headers reach it as /external: includes at /external:W0.
if(NOT MSVC)
    target_compile_options(zaro_platform_sdl PRIVATE -Wno-old-style-cast -Wno-sign-conversion)
endif()
