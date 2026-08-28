find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

# The window and its panels, as a library: zaro-preview links it behind its
# main(), and the GUI tests in app/tests link it in front of Catch2's. A test
# cannot drive a window that only exists inside an executable.
add_library(zaro_app_ui STATIC
    PreviewWindow.h
    ActionRouter.cpp
    ActionRouter.h
    Document.cpp
    Document.h
    PlaybackController.cpp
    PlaybackController.h
    chrome/Bars.cpp
    chrome/Bars.h
    chrome/Widgets.cpp
    chrome/Widgets.h
    chrome/Choices.cpp
    chrome/Choices.h
    chrome/Menus.cpp
    chrome/Menus.h
    commands/Context.h
    commands/Analysis.cpp
    commands/Analysis.h
    commands/Music.cpp
    commands/Music.h
    commands/Multicam.cpp
    commands/Multicam.h
    commands/Media.cpp
    commands/Media.h
    commands/Review.cpp
    commands/Review.h
    commands/Structure.cpp
    commands/Structure.h
    commands/Templates.cpp
    commands/Templates.h
    FrameGrab.h
    MaskOverlay.cpp
    Hotkeys.cpp
    Say.cpp
    MediaBrowser.cpp
    Transcript.cpp
    ProgramMonitor.cpp
    ProgramMonitor.h
    TimelineWidget.cpp
    TimelineWidget.h
    DeliverPanel.cpp
    DeliverPanel.h
    AudioStrip.cpp
    ChannelPanel.cpp
    ChannelPanel.h
    LoudnessPanel.cpp
    LoudnessPanel.h
    AudioStrip.h
    ClipStrip.cpp
    ClipStrip.h
    ColorPalette.cpp
    ColorPalette.h
    FrameThumb.cpp
    FrameThumb.h
    GalleryPanel.cpp
    GalleryPanel.h
    GradeNodes.cpp
    GradeNodes.h
    StemsPanel.cpp
    StemsPanel.h
    ColorWheel.cpp
    ColorWheel.h
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

target_link_libraries(zaro_app_ui
    PUBLIC
        zaro::ui_core
        zaro::platform_qrhi
    zaro::platform_qtext
        zaro::platform_ffmpeg
        zaro::platform_sdl
        zaro::warnings
        Qt6::Widgets
)

target_include_directories(zaro_app_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Qt's headers and moc output trip warnings that are right for our code and
# wrong for a framework this size.
target_compile_options(zaro_app_ui PRIVATE
    -Wno-old-style-cast
    -Wno-sign-conversion
    -Wno-conversion
    -Wno-shadow
)

set_target_properties(zaro_app_ui PROPERTIES AUTOMOC ON)

add_executable(zaro-preview main.cpp)
target_link_libraries(zaro-preview PRIVATE zaro_app_ui zaro::warnings)
target_compile_options(zaro-preview PRIVATE
    -Wno-old-style-cast
    -Wno-sign-conversion
    -Wno-conversion
    -Wno-shadow
)
set_target_properties(zaro-preview PROPERTIES AUTOMOC ON)

