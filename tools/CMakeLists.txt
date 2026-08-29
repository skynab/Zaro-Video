# Cli.h, plus the generated version header it reads. Every tool links this so
# that --version reports the same number the app's title bar shows.
add_library(zaro_tool_cli INTERFACE)
target_include_directories(zaro_tool_cli INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(zaro_tool_cli INTERFACE zaro::version)

add_executable(zaro-probe zaro-probe/main.cpp)
target_link_libraries(zaro-probe PRIVATE zaro_tool_cli zaro::platform_ffmpeg zaro::warnings)

add_executable(zaro-frame zaro-frame/main.cpp)
target_link_libraries(zaro-frame PRIVATE zaro_tool_cli zaro::platform_ffmpeg zaro::warnings)

add_executable(zaro-cut zaro-cut/main.cpp)
target_link_libraries(zaro-cut PRIVATE zaro_tool_cli zaro::platform_ffmpeg zaro::warnings)

# zaro-render constructs a QGuiApplication itself -- Qt's font engine needs one
# even with no window -- so it finds Qt rather than inheriting it. A library
# that built one behind its caller's back would fight whatever the caller had
# already made.
find_package(Qt6 REQUIRED COMPONENTS Gui)

# Links Qt Gui for the text rasteriser. An export that silently dropped every
# title would be the worst kind of wrong: a file that looks finished and is not.
add_executable(zaro-render zaro-render/main.cpp)
target_link_libraries(zaro-render
    PRIVATE zaro_tool_cli zaro::platform_ffmpeg zaro::platform_qtext Qt6::Gui zaro::warnings)

# Interchange needs no media and no toolkit: it is a file conversion.
add_executable(zaro-otio zaro-otio/main.cpp)
target_link_libraries(zaro-otio PRIVATE zaro_tool_cli zaro::core zaro::warnings)

add_executable(zaro-premiere zaro-premiere/main.cpp)
target_link_libraries(zaro-premiere PRIVATE zaro_tool_cli zaro::core zaro::warnings)

add_executable(zaro-play zaro-play/main.cpp)
target_link_libraries(zaro-play
    PRIVATE zaro_tool_cli zaro::platform_ffmpeg zaro::platform_sdl zaro::platform_qrhi zaro::warnings)

# The command-line tools ship alongside the app: the verification scripts drive
# them, and they are the only way to use the renderer without a display.
set(ZARO_TOOLS zaro-probe zaro-frame zaro-cut zaro-render zaro-otio zaro-premiere zaro-play)

include(GNUInstallDirs)
install(TARGETS ${ZARO_TOOLS}
    RUNTIME_DEPENDENCY_SET zaro_runtime_deps
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT runtime)

# On Linux the tools carry their libraries with them, and this is how they find
# them: a relative RPATH, resolved against the executable rather than against
# the machine. It is what makes the same binary work from an unpacked tarball
# in a home directory and from /usr/bin after a .deb -- bin/../lib/cutreel
# either way. See cmake/Packaging.cmake for what fills that directory.
#
# Not on macOS, which uses @loader_path and has its own deployment tool, and
# not on Windows, which has no such concept and looks beside the executable.
if(UNIX AND NOT APPLE)
    set_target_properties(${ZARO_TOOLS} PROPERTIES INSTALL_RPATH "${ZARO_PRIVATE_RPATH}")
endif()
