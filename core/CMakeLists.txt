add_library(zaro_core
    src/Check.cpp
    src/time/Rational.cpp
    src/time/RationalTime.cpp
    src/time/TimeRange.cpp
    src/time/Timecode.cpp
)
add_library(zaro::core ALIAS zaro_core)

target_include_directories(zaro_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(zaro_core PRIVATE zaro::warnings)

target_compile_features(zaro_core PUBLIC cxx_std_20)

set_target_properties(zaro_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# core/ deliberately links nothing from Qt or FFmpeg. Everything here must stay
# runnable headless so the edit engine can be tested, scripted and rendered
# without a window server. Media and GPU code belongs in platform/ behind an
# interface, not in this target.
