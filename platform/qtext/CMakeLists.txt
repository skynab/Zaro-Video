find_package(Qt6 REQUIRED COMPONENTS Gui)

# Gui, not Widgets: the export tool has no window and must not need one to put
# text into a delivered file.
add_library(zaro_platform_qtext src/QtTextRasterizer.cpp)
add_library(zaro::platform_qtext ALIAS zaro_platform_qtext)

target_include_directories(zaro_platform_qtext
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(zaro_platform_qtext
    PUBLIC  zaro::core
    PRIVATE zaro::warnings Qt6::Gui
)
