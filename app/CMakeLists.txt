find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

# The window and its panels, as a library: zaro-preview links it behind its
# main(), and the GUI tests in app/tests link it in front of Catch2's. A test
# cannot drive a window that only exists inside an executable.
add_library(zaro_app_ui STATIC
    ActionRouter.cpp
    ActionRouter.h
    AudioStrip.cpp
    AudioStrip.h
    ChannelPanel.cpp
    ChannelPanel.h
    ClipStrip.cpp
    ClipStrip.h
    ColorPalette.cpp
    ColorPalette.h
    ColorWheel.cpp
    ColorWheel.h
    CurveEditor.cpp
    CurveEditor.h
    DeliverPanel.cpp
    DeliverPanel.h
    Document.cpp
    Document.h
    EffectControls.cpp
    EffectControls.h
    ExportDialog.cpp
    ExportDialog.h
    FrameGrab.h
    FrameThumb.cpp
    FrameThumb.h
    GalleryPanel.cpp
    GalleryPanel.h
    GradeNodes.cpp
    GradeNodes.h
    Hotkeys.cpp
    HueBand.cpp
    HueBand.h
    Icons.cpp
    Icons.h
    LoudnessPanel.cpp
    LoudnessPanel.h
    MaskOverlay.cpp
    MediaBrowser.cpp
    MediaDrag.cpp
    MediaDrag.h
    MixerPanel.cpp
    MixerPanel.h
    PlaybackController.cpp
    PlaybackController.h
    PreviewWindow.cpp
    PreviewWindow.h
    ProgramMonitor.cpp
    ProgramMonitor.h
    ProjectBin.cpp
    ProjectBin.h
    Say.cpp
    ScopesPanel.cpp
    ScopesPanel.h
    SourceMonitor.cpp
    SourceMonitor.h
    StemsPanel.cpp
    StemsPanel.h
    SupportButton.cpp
    SupportButton.h
    Theme.cpp
    Theme.h
    ThumbnailCache.cpp
    ThumbnailCache.h
    TimelineWidget.cpp
    TimelineWidget.h
    Transcript.cpp
    ViewerOverlay.cpp
    ViewerOverlay.h

    chrome/Bars.cpp
    chrome/Bars.h
    chrome/Choices.cpp
    chrome/Choices.h
    chrome/FlowLayout.cpp
    chrome/FlowLayout.h
    chrome/Menus.cpp
    chrome/Menus.h
    chrome/Widgets.cpp
    chrome/Widgets.h

    commands/Analysis.cpp
    commands/Analysis.h
    commands/Context.h
    commands/Media.cpp
    commands/Media.h
    commands/Multicam.cpp
    commands/Multicam.h
    commands/Music.cpp
    commands/Music.h
    commands/Review.cpp
    commands/Review.h
    commands/Structure.cpp
    commands/Structure.h
    commands/Templates.cpp
    commands/Templates.h
)

target_link_libraries(zaro_app_ui
    PUBLIC
        zaro::version
        zaro::ui_core
        zaro::platform_qrhi
        zaro::platform_qtext
        zaro::platform_ffmpeg
        zaro::platform_sdl
        zaro::warnings
        Qt6::Widgets
)

target_include_directories(zaro_app_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(zaro_app_ui PROPERTIES AUTOMOC ON)

# --- Branding -------------------------------------------------------------------
# One directory of artwork, three platforms that each want it in their own
# shape. See resources/branding/README.md for what every file is for.
set(ZARO_BRANDING_DIR "${CMAKE_SOURCE_DIR}/resources/branding")

add_executable(zaro-preview main.cpp)
target_link_libraries(zaro-preview PRIVATE zaro_app_ui zaro::warnings)
set_target_properties(zaro-preview PROPERTIES AUTOMOC ON)

# The window icon, on every platform. Compiled in rather than loaded from a
# file beside the binary, which is a file that goes missing the first time
# somebody moves the executable.
#
# On the executable and not on zaro_app_ui: a Qt resource inside a static
# library is only registered if something drags its initialiser in, and a
# library nobody references by name from main is exactly the case where the
# linker drops it. On the executable there is nothing to drop.
qt_add_resources(zaro-preview "branding"
    PREFIX "/branding"
    BASE "${ZARO_BRANDING_DIR}"
    FILES
        "${ZARO_BRANDING_DIR}/CutReel-16.png"
        "${ZARO_BRANDING_DIR}/CutReel-32.png"
        "${ZARO_BRANDING_DIR}/CutReel-48.png"
        "${ZARO_BRANDING_DIR}/CutReel-64.png"
        "${ZARO_BRANDING_DIR}/CutReel-128.png"
        "${ZARO_BRANDING_DIR}/CutReel-256.png"
        "${ZARO_BRANDING_DIR}/CutReel-512.png")

# The executable's own icon, which is a different thing from the window's: it
# is what Explorer, the taskbar, the desktop shortcut the MSI installs and the
# .zaro document association all display, and none of them ever runs the
# program to ask Qt.
if(WIN32)
    set(ZARO_ICON_ICO "${ZARO_BRANDING_DIR}/CutReel.ico")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/zaro-preview.rc.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/zaro-preview.rc" @ONLY)
    target_sources(zaro-preview PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/zaro-preview.rc")
endif()

# The bundle icon. The file has to be inside Contents/Resources and the plist
# key names it without a path, so both halves are set here; naming one without
# the other is a bundle that shows the generic application icon.
if(APPLE)
    target_sources(zaro-preview PRIVATE "${ZARO_BRANDING_DIR}/CutReel.icns")
    set_source_files_properties("${ZARO_BRANDING_DIR}/CutReel.icns"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    set_target_properties(zaro-preview PROPERTIES MACOSX_BUNDLE_ICON_FILE "CutReel.icns")
endif()


# --- Installation -------------------------------------------------------------
# A bundle on macOS and a windowed executable on Windows, because that is what
# the deployment tools below expect to find and what a double-click on either
# platform expects to launch. On Linux it is a plain binary either way.
set_target_properties(zaro-preview PROPERTIES
    MACOSX_BUNDLE ON
    WIN32_EXECUTABLE ON)

include(GNUInstallDirs)
install(TARGETS zaro-preview
    RUNTIME_DEPENDENCY_SET zaro_runtime_deps
    BUNDLE  DESTINATION .                            COMPONENT runtime
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"    COMPONENT runtime)

# Qt's own libraries and plugins are not on a user's machine, so the archive has
# to carry them. windeployqt and macdeployqt know which ones; nothing else does.
if(WIN32 OR APPLE)
    qt_generate_deploy_app_script(
        TARGET zaro-preview
        OUTPUT_SCRIPT zaro_deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT "${zaro_deploy_script}" COMPONENT runtime)
endif()

# --- Linux ---------------------------------------------------------------------
# Linux has no deployment tool of Qt's, so the two halves are done by hand: the
# shared libraries come from the runtime dependency set in cmake/Packaging.cmake,
# which resolves them the way the loader would, and the plugins come from here,
# because no dependency walk can see them. Qt opens a platform plugin by name at
# startup; nothing links it, so nothing points at it.
if(UNIX AND NOT APPLE)
    set_target_properties(zaro-preview PROPERTIES INSTALL_RPATH "${ZARO_PRIVATE_RPATH}")

    # xcb is what a desktop session wants and offscreen is what a render farm,
    # a container and an ssh session want -- zaro-render selects the second for
    # itself when there is no display. Missing either one is a program that
    # aborts on startup with "could not load the Qt platform plugin", which
    # says nothing about a file having been left out of a package.
    #
    # Added to the same dependency set as the executables: a plugin has
    # libraries of its own, and on Linux they are not always ones the
    # application already pulled in.
    set(zaro_qt_plugins "")
    foreach(plugin
            Qt6::QXcbIntegrationPlugin
            Qt6::QOffscreenIntegrationPlugin
            Qt6::QMinimalIntegrationPlugin)
        if(TARGET ${plugin})
            list(APPEND zaro_qt_plugins ${plugin})
        endif()
    endforeach()
    if(NOT zaro_qt_plugins)
        message(WARNING
            "No Qt platform plugins found to install; the packaged zaro-preview "
            "will not start on a machine without a Qt of its own.")
    else()
        install(IMPORTED_RUNTIME_ARTIFACTS ${zaro_qt_plugins}
            RUNTIME_DEPENDENCY_SET zaro_runtime_deps
            LIBRARY DESTINATION "${ZARO_PRIVATE_LIBDIR}/plugins/platforms"
                    COMPONENT runtime)
    endif()

    # Image formats, for anything the interface draws that is not a PNG -- PNG
    # is built into Qt Gui and needs no plugin.
    foreach(plugin Qt6::QJpegPlugin Qt6::QSvgPlugin)
        if(TARGET ${plugin})
            install(IMPORTED_RUNTIME_ARTIFACTS ${plugin}
                RUNTIME_DEPENDENCY_SET zaro_runtime_deps
                LIBRARY DESTINATION "${ZARO_PRIVATE_LIBDIR}/plugins/imageformats"
                        COMPONENT runtime)
        endif()
    endforeach()

    # And how anything in bin/ finds those plugins. Qt reads qt.conf from beside
    # the executable before it does anything else, and a relative Prefix in it
    # is resolved against that same directory -- so this one file serves the app
    # and all seven tools, wherever the package was unpacked or installed.
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/qt.conf"
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
        COMPONENT runtime)
endif()

# TEMPORARY-SHOT-TARGET
add_executable(zaro_shot ${ZARO_SHOT_SRC})
target_link_libraries(zaro_shot PRIVATE zaro_app_ui Qt6::Widgets)
target_include_directories(zaro_shot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/core/tests)
target_compile_definitions(zaro_shot PRIVATE ZARO_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata/media")
set_target_properties(zaro_shot PROPERTIES AUTOMOC ON)
