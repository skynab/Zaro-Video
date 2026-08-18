add_executable(zaro-probe zaro-probe/main.cpp)
target_link_libraries(zaro-probe PRIVATE zaro::platform_ffmpeg zaro::warnings)

add_executable(zaro-frame zaro-frame/main.cpp)
target_link_libraries(zaro-frame PRIVATE zaro::platform_ffmpeg zaro::warnings)
