find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

add_executable(zaro-preview
    main.cpp
    MaskOverlay.cpp
    MediaBrowser.cpp
    Transcript.cpp
    ProgramMonitor.cpp
    ProgramMonitor.h
    TimelineWidget.cpp
    TimelineWidget.h
    CurveEditor.cpp
    HueBand.cpp
    MixerPanel.cpp
    EffectControls.cpp
    ScopesPanel.cpp
    CurveEditor.h
    HueBand.h
    MixerPanel.h
    EffectControls.h
    ScopesPanel.h
    ExportDialog.cpp
    ExportDialog.h
    ProjectBin.cpp
    ProjectBin.h
    SourceMonitor.cpp
    SourceMonitor.h
    SupportButton.cpp
    SupportButton.h
    Icons.cpp
    Icons.h
    Theme.cpp
    Theme.h
    ViewerOverlay.cpp
    ViewerOverlay.h
)

target_link_libraries(zaro-preview
    PRIVATE
        zaro::ui_core
        zaro::platform_qrhi
    zaro::platform_qtext
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
