add_executable(zaro-probe zaro-probe/main.cpp)
target_link_libraries(zaro-probe PRIVATE zaro::platform_ffmpeg zaro::warnings)

add_executable(zaro-frame zaro-frame/main.cpp)
target_link_libraries(zaro-frame PRIVATE zaro::platform_ffmpeg zaro::warnings)

add_executable(zaro-cut zaro-cut/main.cpp)
target_link_libraries(zaro-cut PRIVATE zaro::platform_ffmpeg zaro::warnings)

add_executable(zaro-render zaro-render/main.cpp)
target_link_libraries(zaro-render PRIVATE zaro::platform_ffmpeg zaro::warnings)

add_executable(zaro-play zaro-play/main.cpp)
target_link_libraries(zaro-play PRIVATE zaro::platform_ffmpeg zaro::platform_sdl zaro::warnings)
