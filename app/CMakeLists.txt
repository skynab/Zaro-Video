find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

add_executable(zaro-preview
    main.cpp
    ProgramMonitor.cpp
    ProgramMonitor.h
    TimelineWidget.cpp
    TimelineWidget.h
    EffectControls.cpp
    EffectControls.h
    ExportDialog.cpp
    ExportDialog.h
    ProjectBin.cpp
    ProjectBin.h
)

target_link_libraries(zaro-preview
    PRIVATE
        zaro::ui_core
        zaro::platform_qrhi
        zaro::platform_ffmpeg
        zaro::platform_sdl
        zaro::warnings
        Qt6::Widgets
)

# Qt's headers and moc output trip warnings that are right for our code and
# wrong for a framework this size.
target_compile_options(zaro-preview PRIVATE
    -Wno-old-style-cast
    -Wno-sign-conversion
    -Wno-conversion
    -Wno-shadow
)

set_target_properties(zaro-preview PROPERTIES AUTOMOC ON)
