add_executable(zaro-probe zaro-probe/main.cpp)
target_link_libraries(zaro-probe PRIVATE zaro::platform_ffmpeg zaro::warnings)

add_executable(zaro-frame zaro-frame/main.cpp)
target_link_libraries(zaro-frame PRIVATE zaro::platform_ffmpeg zaro::warnings)

add_executable(zaro-cut zaro-cut/main.cpp)
target_link_libraries(zaro-cut PRIVATE zaro::platform_ffmpeg zaro::warnings)

# zaro-render constructs a QGuiApplication itself -- Qt's font engine needs one
# even with no window -- so it finds Qt rather than inheriting it. A library
# that built one behind its caller's back would fight whatever the caller had
# already made.
find_package(Qt6 REQUIRED COMPONENTS Gui)

# Links Qt Gui for the text rasteriser. An export that silently dropped every
# title would be the worst kind of wrong: a file that looks finished and is not.
add_executable(zaro-render zaro-render/main.cpp)
target_link_libraries(zaro-render
    PRIVATE zaro::platform_ffmpeg zaro::platform_qtext Qt6::Gui zaro::warnings)

add_executable(zaro-play zaro-play/main.cpp)
target_link_libraries(zaro-play
    PRIVATE zaro::platform_ffmpeg zaro::platform_sdl zaro::platform_qrhi zaro::warnings)
