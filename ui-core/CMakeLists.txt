# Presentation logic with no toolkit in it.
#
# A timeline's hard parts are arithmetic -- zoom anchoring, hit-testing, culling
# -- and none of it needs a window to be correct or to be tested. Keeping it
# here means the Qt layer above is mostly painting, and means these can be
# exercised in CI on a machine with no display.
add_library(zaro_ui_core src/TimelineLayout.cpp)
add_library(zaro::ui_core ALIAS zaro_ui_core)

target_include_directories(zaro_ui_core
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(zaro_ui_core
    PUBLIC  zaro::core
    PRIVATE zaro::warnings
)
