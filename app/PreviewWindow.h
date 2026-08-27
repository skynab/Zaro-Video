// The preview window: the application's main window.
//
// A program monitor over the GPU compositor, with transport. This is the point
// at which the pipeline built in Phase 3 becomes something a person can look
// at: the composited texture goes straight from the compositor to the screen
// without ever being read back into system memory.
//
// Playback here is render-on-demand against the audio clock rather than a
// pre-rendered frame queue. The queue in PlaybackScheduler exists because a
// slow renderer needs somewhere to work ahead; a GPU that composites 1080p in
// under two milliseconds does not, and asking it for the frame the clock is
// currently on is both simpler and lower latency.
//
// It lives in a header rather than inside main.cpp so that the GUI tests in
// app/tests can drive the same window a person drives: a class in an anonymous
// namespace in main.cpp is reachable from nothing. The bodies are still inline
// here because this move is meant to be verbatim -- splitting them out into a
// .cpp is a separate step, so that a reviewer of either one is reading a change
// that does only what it says.
#pragma once

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStringList>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "zaro/core/Check.h"
#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/core/edit/Sync.h"
#include "zaro/core/io/CubeLut.h"
#include "zaro/core/io/OtioIo.h"
#include "zaro/core/io/ProjectIo.h"
#include "zaro/core/io/ProjectLock.h"
#include "zaro/core/io/Relink.h"
#include "zaro/core/io/ReviewNotes.h"
#include "zaro/core/io/SubtitleIo.h"
#include "zaro/core/playback/Transport.h"
#include "zaro/core/render/AudioGraph.h"
#include "zaro/core/render/BakeLut.h"
#include "zaro/core/render/ColorPipeline.h"
#include "zaro/core/render/Compare.h"
#include "zaro/core/render/PathRaster.h"
#include "zaro/core/render/Reframe.h"
#include "zaro/core/render/Remix.h"
#include "zaro/core/render/RenderCache.h"
#include "zaro/core/render/RenderGraph.h"
#include "zaro/core/render/SceneDetect.h"
#include "zaro/core/render/Scopes.h"
#include "zaro/core/render/ShotMatch.h"
#include "zaro/core/render/Stabilise.h"
#include "zaro/core/render/ToneMap.h"
#include "zaro/core/render/Tracker.h"
#include "zaro/core/time/Timecode.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"
#include "zaro/platform/qtext/QtTextRasterizer.h"
#include "zaro/platform/sdl/AudioSink.h"
#include "zaro/ui/Actions.h"
#include "zaro/ui/Keymap.h"

#include "ChannelPanel.h"
#include "ClipStrip.h"
#include "ColorPalette.h"
#include "CurveEditor.h"
#include "DeliverPanel.h"
#include "EffectControls.h"
#include "ExportDialog.h"
#include "FrameThumb.h"
#include "GalleryPanel.h"
#include "GradeNodes.h"
#include "Hotkeys.h"
#include "Icons.h"
#include "LoudnessPanel.h"
#include "MaskOverlay.h"
#include "MediaBrowser.h"
#include "MixerPanel.h"
#include "ProgramMonitor.h"
#include "ProjectBin.h"
#include "Say.h"
#include "ScopesPanel.h"
#include "SourceMonitor.h"
#include "StemsPanel.h"
#include "SupportButton.h"
#include "Theme.h"
#include "TimelineWidget.h"
#include "Transcript.h"
#include "ViewerOverlay.h"

namespace zaro::app {

// Transport glyphs. Characters rather than an icon font: nothing is vendored
// here, and a glyph that renders as a colour emoji on one platform and an
// empty box on another is worse than a shape every font has.
constexpr const char* kPlayGlyph = "\u25B6";         // BLACK RIGHT-POINTING TRIANGLE
constexpr const char* kPauseGlyph = "\u275A\u275A";  // HEAVY VERTICAL BAR, twice

/// The workspaces, in the order they appear on the tool bar.
inline const QStringList kWorkspaces{"Edit", "Color", "Audio", "Deliver"};

/// What the status line says it is running on.
inline const QString kPlatformLabel = QSysInfo::prettyProductName();

/// Where Donate goes. One constant, because a project's funding page moves and
/// the button should not have to be found again when it does. Until there is a
/// funding page this is the project's own, which is at least somewhere a person
/// who wants to help can start.
inline const QString kSupportUrl = "https://github.com/skynab/Zaro-Video";

// No Q_OBJECT: this declares no signals or slots of its own, and
// QMetaObject::invokeMethod with a lambda needs only a QObject to bind the
// call to. Adding it in a .cpp would also require including its moc output.

class PreviewWindow : public QWidget {
public:
    PreviewWindow(model::Project project, io::LoadedProject loaded, std::string path)
        : project_{std::move(project)}, loaded_{std::move(loaded)}, path_{std::move(path)} {
        sequenceId_ = project_.activeSequence();
        // What was just loaded is what is on disk, by definition.
        commands_.markSaved();

        monitor_ = new app::ProgramMonitor(this);
        // Narrow enough that a source and a program fit side by side in a
        // window somebody has not maximised, without the splitter having to
        // take the difference out of the parameter column and clip it. Only the
        // width gives: two monitors sit beside each other, so they share the
        // well's height rather than dividing it, and a monitor too short to
        // judge a frame on is the thing this floor exists to prevent.
        monitor_->setMinimumSize(240, 270);

        // The mask handles live on a transparent widget over the picture, so
        // the renderer never has to know about them and the widget that draws
        // them is the one that receives the clicks.
        maskOverlay_ = new app::MaskOverlay(monitor_, monitor_);
        monitor_->installEventFilter(this);
        connect(maskOverlay_, &app::MaskOverlay::edited, this, [this] {
            renderCache_.clear();
            monitor_->update();
            timeline_->update();
            effects_->refresh();
            updateCacheBar();
            updateTitle();
        });

        // The burn-in: what the frame is, over the frame. Below the mask
        // handles, and transparent to the mouse, so it never takes a click the
        // mask editor wanted.
        viewerOverlay_ = new app::ViewerOverlay(monitor_, monitor_);
        maskOverlay_->raise();

        timecode_ = new QLabel(this);
        timecode_->setObjectName("timecode-big");
        // Ask the system for its fixed-width family rather than naming one:
        // a missing family costs a slow alias lookup and silently falls back.
        QFont monospace = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        monospace.setPointSize(17);
        timecode_->setFont(monospace);
        remaining_ = new QLabel(this);
        QFont smallMonospace = monospace;
        smallMonospace.setPointSize(11);
        remaining_->setFont(smallMonospace);
        remaining_->setProperty("muted", true);
        playButton_ = new QPushButton(kPlayGlyph, this);
        playButton_->setToolTip("Play or pause (Space)");
        playButton_->setProperty("accent", true);
        playButton_->setFixedSize(46, 30);
        scrubber_ = new QSlider(Qt::Horizontal, this);

        timeline_ = new app::TimelineWidget(this);
        effects_ = new app::EffectControls(this);
        connect(effects_, &app::EffectControls::drawMaskRequested, maskOverlay_,
                &app::MaskOverlay::setDrawing);
        connect(maskOverlay_, &app::MaskOverlay::drawingChanged, effects_,
                &app::EffectControls::setDrawingMask);
        connect(effects_, &app::EffectControls::trackMaskRequested, this, [this] { trackMask(); });
        connect(effects_, &app::EffectControls::pinRequested, this, [this] {
            if (auto pinned = pinToClipBelow(); !pinned) {
                app::say(this, "Pin", QString::fromStdString(pinned.error().message()));
            }
        });
        connect(effects_, &app::EffectControls::unpinRequested, this,
                [this] { static_cast<void>(pinTo(model::ClipId{})); });
        connect(effects_, &app::EffectControls::stabiliseRequested, this, [this] { stabilise(); });
        connect(effects_, &app::EffectControls::reframeRequested, this, [this] {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            auto framed = reframeClip();
            QApplication::restoreOverrideCursor();
            if (!framed) {
                app::say(this, "Auto-reframe", QString::fromStdString(framed.error().message()));
                return;
            }
            QString said = QString("Reframed %1 frames, scaled to %2%.")
                               .arg(framed->measured)
                               .arg(framed->scale * 100.0, 0, 'f', 0);
            if (!framed->reason.empty()) {
                said += QString("\n%1").arg(QString::fromStdString(framed->reason));
            }
            app::say(this, "Auto-reframe", said);
        });
        connect(effects_, &app::EffectControls::clearStabilisationRequested, this,
                [this] { clearStabilisation(); });
        scopes_ = new app::ScopesPanel(this);
        mixer_ = new app::MixerPanel(this);
        // 250 was under what the parameter rows actually measure, so the
        // splitter was free to squeeze the column until the value fields ran
        // off the edge of it. Now it is the width the content needs.
        effects_->setMinimumWidth(300);
        effects_->setMaximumWidth(330);

        // Monitor and parameters side by side, transport under them, timeline
        // across the bottom.
        bin_ = new app::ProjectBin(this);
        // The width the design fixes the media pane at. Fixed rather than a
        // range because the row inside it is fixed too -- a 64-pixel thumbnail,
        // two lines of type and a dot -- and the pane has nothing that would
        // use the extra space if it were given any.
        bin_->setFixedWidth(296);

        // Its own row of buttons -- In, Out, Subclip, Insert, Over -- is what
        // sets this, not the picture: below this width the labels start losing
        // letters, and a button reading "nsert" is worse than a narrow picture.
        thumb_ = new app::FrameThumb(this);
        loudness_ = new app::LoudnessPanel(this);
        stems_ = new app::StemsPanel(this);
        channel_ = new app::ChannelPanel(this);
        channel_->setMinimumWidth(280);
        channel_->setMaximumWidth(320);

        gallery_ = new app::GalleryPanel(this);
        gallery_->setFixedWidth(236);
        clipStrip_ = new app::ClipStrip(this);
        nodes_ = new app::GradeNodes(this);
        palette_ = new app::ColorPalette(this);
        palette_->setFixedHeight(212);

        source_ = new app::SourceMonitor(this);
        source_->setMinimumWidth(280);

        // Splitters rather than fixed layouts: panel sizes are a matter of what
        // someone is doing at the time, and the arrangement is remembered
        // between sessions.
        //
        // Everything the window can do lives in a menu. It used to live in a
        // row of eighteen buttons under the picture, which was fine while there
        // were four of them: a menu bar is the structure that says which of
        // them belong together, and it costs the transport nothing.
        // The keymap first: menus read their shortcuts from it as they are
        // built, so a customised binding is on the item the first time it is
        // drawn rather than after a refresh nobody triggers.
        loadKeymap();
        bindPlaybackActions();
        buildMenus();

        // Source and program side by side, each shown or not on its own.
        //
        // Two toggles rather than two tabs, as the design draws them: comparing
        // the clip you are about to place against the cut you are placing it
        // into is the whole reason both monitors exist, and a stack can only
        // ever answer "which one" -- never "both". Either can be off, including
        // both, because a window given over to the timeline is a real way to
        // work and the well is the largest thing to reclaim.
        viewerWell_ = new QWidget(this);
        viewerWell_->setObjectName("viewer-well");
        viewerWell_->setAttribute(Qt::WA_StyledBackground, true);
        auto* wellRow = new QHBoxLayout(viewerWell_);
        wellRow->setContentsMargins(12, 12, 12, 12);
        wellRow->setSpacing(12);
        wellRow->addWidget(source_, 1);
        wellRow->addWidget(monitor_, 1);
        // Scopes beside the picture rather than off in the parameter column,
        // which is where the design puts them and where they are read: an
        // instrument is compared against the frame it measures, and a glance
        // that crosses the whole window is a glance nobody takes.
        scopes_->setFixedWidth(330);
        wellRow->addWidget(scopes_);
        noMonitorLabel_ = new QLabel("No monitor shown \u2014 turn on Source or Program", this);
        noMonitorLabel_->setAlignment(Qt::AlignCenter);
        noMonitorLabel_->setProperty("muted", true);
        wellRow->addWidget(noMonitorLabel_, 1);

        auto* programColumn = new QWidget(this);
        auto* programLayout = new QVBoxLayout(programColumn);
        programLayout->setContentsMargins(0, 0, 0, 0);
        programLayout->setSpacing(0);
        viewerBar_ = buildViewerBar();
        programLayout->addWidget(viewerBar_);
        programLayout->addWidget(viewerWell_, 1);
        // The console takes the centre in Audio, where the picture is in every
        // other workspace: a mix is read across ten channels at once, and a
        // column of strips squeezed into a side panel is a column nobody can
        // find a fader in.
        programLayout->addWidget(mixer_, 1);
        programLayout->addWidget(clipStrip_);
        programLayout->addWidget(buildTransportBar());

        topSplitter_ = new QSplitter(Qt::Horizontal, this);
        topSplitter_->setHandleWidth(1);
        auto* leftColumn = new QWidget(this);
        leftColumn->setObjectName("audio-side");
        leftColumn->setFixedWidth(262);
        auto* leftLayout = new QVBoxLayout(leftColumn);
        leftLayout->setContentsMargins(0, 0, 0, 0);
        leftLayout->setSpacing(0);
        leftLayout->addWidget(thumb_);
        leftLayout->addWidget(loudness_);
        leftLayout->addWidget(stems_);
        leftLayout->addStretch(1);
        audioSide_ = leftColumn;

        topSplitter_->addWidget(audioSide_);
        topSplitter_->addWidget(gallery_);
        topSplitter_->addWidget(bin_);
        topSplitter_->addWidget(programColumn);
        // Scopes share the parameter column: they are read while grading, and
        // grading is done with the parameters in reach.
        // The grade chain sits on top of the parameters it navigates, in one
        // column rather than as a splitter pane: the node strip is a fixed
        // 118 pixels and giving it a drag handle would invite somebody to
        // squash a picture that has nothing to gain from being shorter.
        auto* gradeColumn = new QWidget(this);
        auto* gradeLayout = new QVBoxLayout(gradeColumn);
        gradeLayout->setContentsMargins(0, 0, 0, 0);
        gradeLayout->setSpacing(0);
        nodesBox_ = new QWidget(gradeColumn);
        nodesBox_->setObjectName("grade-nodes-box");
        auto* nodesLayout = new QVBoxLayout(nodesBox_);
        nodesLayout->setContentsMargins(12, 10, 12, 10);
        nodesLayout->addWidget(nodes_);
        gradeLayout->addWidget(nodesBox_);
        gradeLayout->addWidget(effects_, 1);

        auto* rightColumn = new QSplitter(Qt::Vertical, this);
        rightColumn->setHandleWidth(1);
        rightColumn->addWidget(gradeColumn);
        rightColumn->addWidget(channel_);
        rightColumn->setStretchFactor(0, 4);
        rightColumn->setStretchFactor(1, 4);
        // Floors, so no panel can be squeezed to a sliver by its neighbours. A
        // scope four pixels tall or a mixer with no meter is worse than one
        // that pushes the window wider: it looks like a broken panel rather
        // than a small one.
        effects_->setMinimumHeight(220);
        scopes_->setMinimumHeight(150);
        mixer_->setMinimumHeight(190);
        topSplitter_->addWidget(rightColumn);
        topSplitter_->setStretchFactor(3, 3);

        // Picking a shot in the strip is the Color workspace's way of moving
        // about: it selects the clip and puts the playhead inside it, so the
        // monitor, the scopes and every parameter panel are all looking at the
        // same shot.
        connect(clipStrip_, &app::ClipStrip::chosen, this,
                [this](zaro::model::TrackId track, zaro::model::ClipId clip) {
                    const model::Sequence* sequence = liveSequence();
                    const model::Track* found =
                        sequence != nullptr ? sequence->findTrack(track) : nullptr;
                    const model::Clip* shot = found != nullptr ? found->find(clip) : nullptr;
                    if (shot == nullptr) {
                        return;
                    }
                    timeline_->selectOnly(track, clip);
                    setPosition(shot->timelineRange.start());
                });

        connect(mixer_, &app::MixerPanel::pickedChanged, this,
                [this](zaro::model::TrackId track) { channel_->setTrack(track); });
        connect(channel_, &app::ChannelPanel::edited, this, [this] {
            mixer_->refresh();
            timeline_->update();
            updateTitle();
        });
        connect(loudness_, &app::LoudnessPanel::measureRequested, this,
                [this] { measureProgramme(); });

        // Picking a stem is how somebody asks "where is the music": it goes to
        // the first clip carrying that role and selects it, so the mixer and
        // the channel panel are looking at the same sound.
        connect(stems_, &app::StemsPanel::stemChosen, this,
                [this](zaro::model::TrackId track, zaro::model::ClipId clip,
                       zaro::time::RationalTime at) {
                    timeline_->selectOnly(track, clip);
                    setPosition(at);
                });

        connect(nodes_, &app::GradeNodes::stageChosen, this,
                [this](int stage) { effects_->revealStage(stage); });

        connect(palette_, &app::ColorPalette::edited, this, [this] {
            renderCache_.clear();
            monitor_->update();
            timeline_->update();
            effects_->refresh();
            clipStrip_->refresh();
            refreshGradeChain();
            refreshInstruments();
            updateTitle();
        });

        // A still is a reference frame: grabbing one records where it came
        // from, and picking one points the split compare at that moment.
        connect(gallery_, &app::GalleryPanel::grabRequested, this, [this] { grabStill(); });
        connect(gallery_, &app::GalleryPanel::stillChosen, this,
                [this](zaro::time::RationalTime at) {
                    setCompareMode(render::CompareMode::Split);
                    setComparing(true, at);
                    monitor_->update();
                });
        connect(gallery_, &app::GalleryPanel::lutChosen, this,
                [this](const QString& path) { applyLookToSelection(path); });

        connect(bin_, &app::ProjectBin::openRequested, this, [this](zaro::model::MediaRefId id) {
            if (const model::MediaRef* ref = project_.findMedia(id)) {
                source_->load(*ref);
                // Opening a clip is a request to look at it.
                setSourceShown(true);
            }
        });
        connect(bin_, &app::ProjectBin::openSubclipRequested, this,
                [this](zaro::model::SubclipId id) {
                    const model::Subclip* subclip = project_.findSubclip(id);
                    if (subclip == nullptr) {
                        return;
                    }
                    if (const model::MediaRef* ref = project_.findMedia(subclip->source)) {
                        source_->loadMarked(*ref, subclip->range);
                        setSourceShown(true);
                    }
                });
        connect(bin_, &app::ProjectBin::colorChanged, this, [this] {
            // The media source resolved each file's curve when it opened, so
            // correcting one means reopening -- the same swap the proxy toggle
            // makes, for the same reason.
            renderCache_.clear();
            if (Status reopened = reopenMedia(); !reopened) {
                app::warn(this, "Interpret", QString::fromStdString(reopened.error().toString()));
            }
            monitor_->update();
            updateCacheBar();
        });
        connect(bin_, &app::ProjectBin::replaceRequested, this,
                [this](zaro::model::MediaRefId id) { replaceSelectedSource(id); });
        connect(source_, &app::SourceMonitor::subclipRequested, this, [this] { makeSubclip(); });
        connect(source_, &app::SourceMonitor::insertRequested, this,
                [this] { placeFromSource(edit::PlaceMode::Insert); });
        connect(source_, &app::SourceMonitor::overwriteRequested, this,
                [this] { placeFromSource(edit::PlaceMode::Overwrite); });
        connect(bin_, &app::ProjectBin::edited, this, [this] {
            scrubber_->setRange(0, static_cast<int>(liveSequence()->duration().frames()));
            timeline_->update();
            monitor_->update();
            refresh();
        });

        mainSplitter_ = new QSplitter(Qt::Vertical, this);
        mainSplitter_->setHandleWidth(1);
        mainSplitter_->addWidget(topSplitter_);
        timelinePane_ = buildTimelinePane();
        mainSplitter_->addWidget(timelinePane_);
        // The grading palette takes the timeline's place in Color. Not beside
        // it: the design gives that workspace no timeline at all, because what
        // a colourist moves through is shots, and the strip under the viewer is
        // the list of those.
        mainSplitter_->addWidget(palette_);
        mainSplitter_->setStretchFactor(0, 3);
        mainSplitter_->setStretchFactor(1, 2);

        // Deliver is not a rearrangement of the edit panels, it is a different
        // screen: presets, settings and a render queue, with no timeline. So it
        // is a page rather than a workspace layout, and the workspace tabs
        // switch between them.
        deliver_ = new app::DeliverPanel(this);
        deliver_->setProject(&project_, sequenceId_);
        connect(deliver_, &app::DeliverPanel::queueChanged, this, [this] { updateChrome(); });

        workspaceStack_ = new QStackedWidget(this);
        workspaceStack_->addWidget(mainSplitter_);
        workspaceStack_->addWidget(deliver_);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(buildTitleBar());
        layout->addWidget(buildToolBar());
        layout->addWidget(workspaceStack_, 1);
        layout->addWidget(buildStatusBar());

        connect(timeline_, &app::TimelineWidget::selectionChanged, effects_,
                &app::EffectControls::setSelection);
        // Kept here too: syncing acts on the clip somebody has picked, and the
        // timeline is where picking happens.
        connect(timeline_, &app::TimelineWidget::selectionChanged, this,
                [this](model::TrackId track, model::ClipId clip) {
                    selectedTrack_ = track;
                    selectedClip_ = clip;
                    palette_->setSelection(track, clip);
                    clipStrip_->setSelection(track, clip);
                    refreshGradeChain();
                    maskOverlay_->setTarget(&project_, sequenceId_, track, clip, &commands_);
                });
        connect(scopes_, &app::ScopesPanel::measurementNeeded, this,
                [this] { refreshInstruments(); });
        connect(mixer_, &app::MixerPanel::edited, this, [this] {
            // Mute and solo change the picture as well as the sound: a muted
            // video track stops being composited.
            monitor_->update();
            timeline_->update();
            refreshInstruments();
        });
        connect(effects_, &app::EffectControls::keyframesChanged, this,
                [this] { timeline_->update(); });
        connect(effects_, &app::EffectControls::edited, this, [this] {
            // A parameter change alters the picture at the current playhead.
            monitor_->update();
            timeline_->update();
            refreshInstruments();
        });

        // The two panels drive each other: scrubbing the timeline moves the
        // picture, and playback moves the playhead.
        connect(timeline_, &app::TimelineWidget::playheadMoved, this,
                [this](const time::RationalTime& position) {
                    stop();
                    setPosition(position);
                });
        connect(timeline_, &app::TimelineWidget::viewChanged, this, [this] {
            updateCacheBar();
            updateChrome();
        });
        connect(timeline_, &app::TimelineWidget::toolChanged, this, [this] { updateChrome(); });
        connect(timeline_, &app::TimelineWidget::snapChanged, this, [this] { updateChrome(); });
        connect(timeline_, &app::TimelineWidget::edited, this, [this] {
            // Undo can change a clip's parameters as well as its position, so
            // the panel has to re-read rather than trust what it last wrote.
            effects_->refresh();
            mixer_->refresh();
            // An edit can change the duration, and can change what is under the
            // playhead, so both the scrubber and the picture need refreshing.
            scrubber_->setRange(0, static_cast<int>(liveSequence()->duration().frames()));
            monitor_->update();
            refresh();
            // An edit invalidates whatever it shows on, so the bar has to be
            // re-read rather than left claiming a range that no longer renders
            // to what is cached.
            updateCacheBar();
            updateTitle();
        });

        connect(playButton_, &QPushButton::clicked, this, [this] { togglePlay(); });
        connect(scrubber_, &QSlider::sliderMoved, this, [this](int value) {
            // Scrubbing stops playback: the playhead is being driven by hand,
            // and having the clock fight it is what makes scrubbing feel loose.
            stop();
            setPosition(time::RationalTime{value, liveSequence()->frameRate()});
        });

        // Meters at twenty a second. Faster is invisible on a meter with a peak
        // hold, and each tick is a mix of a short block when the transport is
        // stopped.
        meterTimer_ = new QTimer(this);
        meterTimer_->setInterval(50);
        connect(meterTimer_, &QTimer::timeout, this, [this] { updateMeters(); });
        meterTimer_->start();

        clockTimer_ = new QTimer(this);
        // Faster than any display refresh, so the playhead is never waiting on
        // the timer rather than on the clock.
        clockTimer_->setInterval(4);
        connect(clockTimer_, &QTimer::timeout, this, [this] { followClock(); });

        updateTitle();
        // Every thirty seconds, and only when something has changed. A timer
        // that writes regardless would rewrite an untouched project all day;
        // one that writes on every edit would stall a drag on disk.
        autosaveTimer_ = new QTimer(this);
        autosaveTimer_->setInterval(30000);
        connect(autosaveTimer_, &QTimer::timeout, this, [this] { autosave(); });
        autosaveTimer_->start();
        restoreWorkspace();
        refresh();
    }

    ~PreviewWindow() override { shutDown(); }

    [[nodiscard]] app::ProgramMonitor* monitor() const { return monitor_; }
    [[nodiscard]] app::TimelineWidget* timeline() const { return timeline_; }
    [[nodiscard]] app::SourceMonitor* sourceMonitor() const { return source_; }
    [[nodiscard]] app::EffectControls* effects() const { return effects_; }
    [[nodiscard]] app::ScopesPanel* scopes() const { return scopes_; }
    [[nodiscard]] app::MixerPanel* mixer() const { return mixer_; }
    [[nodiscard]] app::ChannelPanel* channel() const { return channel_; }
    [[nodiscard]] app::DeliverPanel* deliver() const { return deliver_; }
    [[nodiscard]] render::AudioSource& media() { return *media_; }
    [[nodiscard]] render::SourceFrameProvider* frames() { return media_.get(); }
    [[nodiscard]] render::FrameSource& frameSource() { return *media_; }
    [[nodiscard]] app::ProjectBin* bin() { return bin_; }
    [[nodiscard]] Status reopenMedia() { return openMedia(); }

    /// Re-seat everything that holds a pointer to the active sequence.
    ///
    /// Adding a sequence reallocates the project's vector of them, so anything
    /// holding a pointer into it is looking at freed memory. This window looks
    /// its own up by id; the monitor is handed one, so it has to be told again.
    /// Show a different sequence of the same project.
    ///
    /// The playhead goes back to the start rather than being carried across:
    /// a position means "this far into this sequence", and keeping the number
    /// while changing what it counts into is how a window ends up parked past
    /// the end of something.
    void setActiveSequence(model::SequenceId id) {
        if (project_.findSequence(id) == nullptr) {
            return;
        }
        project_.setActiveSequence(id);
        sequenceId_ = id;
        position_ = time::RationalTime{0, project_.findSequence(id)->frameRate()};
        selectedTrack_ = model::TrackId{};
        selectedClip_ = model::ClipId{};
        renderCache_.clear();
        // The panels hold which sequence they are about, so they have to be
        // told too: a parameter panel still pointed at the sequence that has
        // gone shows nothing and disables everything, which looks like the
        // clip being unselectable rather than the panel being lost.
        effects_->setProject(&project_, id, &commands_);
        bin_->setProject(&project_, id, &commands_);
        rebindSequence();
        refresh();
    }

    void rebindSequence() {
        if (deliver_ != nullptr) {
            deliver_->setProject(&project_, sequenceId_);
        }
        monitor_->setSource(liveSequence(), media_.get());
        monitor_->setNesting(&project_, media_.get());
        monitor_->setRenderCache(&renderCache_);
        monitor_->setTextRasterizer(&text_);
        timeline_->setProject(&project_, sequenceId_, &commands_);
        monitor_->update();
    }

    /// Feed the mixer's meters.
    ///
    /// While playing they come from the thread that is actually producing the
    /// audio, so they show what is being heard. Stopped, a short block is mixed
    /// at the playhead: a mixer whose meters die whenever the transport stops
    /// cannot be used to set a level, which is most of what a mixer is for.
    void updateMeters() {
        if (mixer_ == nullptr || liveSequence() == nullptr || !mixer_->isVisible()) {
            return;
        }
        if (playing_) {
            render::AudioGraph::Meters copy;
            {
                const std::lock_guard<std::mutex> guard{meterMutex_};
                copy = latestMeters_;
            }
            mixer_->setMeters(copy);
            return;
        }
        if (media_ == nullptr) {
            return;
        }
        render::AudioGraph meterMix{*media_};
        const auto& audioRate = liveSequence()->audioSampleRate();
        constexpr std::int64_t kBlock = 1024;
        if (meterMix.mix(*liveSequence(), position_.rescaledTo(audioRate), kBlock, 2)) {
            mixer_->setMeters(meterMix.meters());
        }
    }

    [[nodiscard]] const model::Sequence* sequence() const { return liveSequence(); }
    [[nodiscard]] model::Project& project() { return project_; }

    /// Block until the background peak generation has finished and its results
    /// have been delivered. For the self-test, which would otherwise capture
    /// the timeline before any waveform arrived.
    void waitForWaveforms() {
        if (waveformThread_.joinable()) {
            waveformThread_.join();
        }
        // The results are handed over through the event loop, so they are not
        // on screen until it has been given a chance to run.
        QApplication::processEvents();
    }
    /// The active sequence, looked up each time. A handful of sequences and a
    /// linear scan: cheaper than any of the ways of getting this wrong.
    [[nodiscard]] const model::Sequence* liveSequence() const {
        return project_.findSequence(sequenceId_);
    }

    [[nodiscard]] edit::CommandStack& commands() { return commands_; }
    [[nodiscard]] render::RenderCache& renderCache() { return renderCache_; }

    /// Match the selected clip to the frame being held as the reference.
    ///
    /// Returns the match so a caller can say what happened. Nothing is applied
    /// when the two shots are too unalike: an automatic grade that is confidently
    /// wrong is worse than none, and the person looking at both frames is better
    /// placed to decide than a distance measure is.
    Result<render::ShotMatch> matchToReference() {
        const model::Sequence* sequence = liveSequence();
        const model::Track* track =
            sequence != nullptr ? sequence->findTrack(selectedTrack_) : nullptr;
        const model::Clip* clip = track != nullptr ? track->find(selectedClip_) : nullptr;
        if (clip == nullptr || media_ == nullptr) {
            return Error{ErrorCode::InvalidData, "select the clip to match first"};
        }
        if (!comparing_) {
            return Error{ErrorCode::InvalidData, "hold a frame to match against first"};
        }

        // Both frames composited the same way, through the CPU graph the
        // comparison already uses -- so what is matched is what is on screen.
        render::RenderGraph graph{*media_};
        graph.setProject(&project_);
        graph.setTextRasterizer(&text_);
        graph.setRenderCache(&renderCache_);

        render::RgbaImage reference;
        if (Status status = graph.compositeInto(*sequence, referenceAt_, reference); !status) {
            return status.error();
        }
        render::RgbaImage current;
        if (Status status = graph.compositeInto(*sequence, position_, current); !status) {
            return status.error();
        }

        auto match = render::matchShot(reference, current);
        if (!match) {
            return match.error();
        }
        if (!match->usable) {
            return match;
        }

        auto built = edit::makeSetWheels(project_, {sequence->id(), selectedTrack_}, selectedClip_,
                                         match->wheels);
        if (!built) {
            return built.error();
        }
        commands_.execute(project_, std::move(*built));
        commands_.breakMerge();
        renderCache_.clear();
        monitor_->update();
        updateCacheBar();
        updateTitle();
        return match;
    }

    /// What tracking a mask through the shot came to.
    struct MaskTrack {
        int frames{0};
        double confidence{1.0};
        /// Set when the track stopped early, saying why. The keyframes found
        /// before it stopped are kept: a track that held for two seconds and
        /// then lost the thing it was on is worth two seconds of keyframes and
        /// a note, not a refusal.
        std::string stopped;
    };

    /// Follow the selected clip's mask through the rest of the clip.
    ///
    /// **Frame to frame, not against the first frame.** A reference frame does
    /// not drift, but it also stops matching the moment the thing turns, moves
    /// under a different light, or is partly covered -- which is most shots
    /// worth tracking. Frame to frame follows all of that and accumulates a
    /// little error instead, which is the trade every tracker makes and the one
    /// people can correct by hand afterwards.
    ///
    /// **On the composited picture, not on the decoded source.** The mask lives
    /// in output coordinates over whatever is on screen, so what it has to
    /// follow is what is on screen.
    Result<MaskTrack> trackMaskForward() {
        const model::Sequence* sequence = liveSequence();
        const model::Track* track =
            sequence != nullptr ? sequence->findTrack(selectedTrack_) : nullptr;
        const model::Clip* clip = track != nullptr ? track->find(selectedClip_) : nullptr;
        if (clip == nullptr || media_ == nullptr) {
            return Error{ErrorCode::InvalidData, "select the clip whose mask should be tracked"};
        }
        if (!clip->mask.isSet()) {
            return Error{ErrorCode::InvalidData, "that clip has no mask to track"};
        }
        const time::RationalTime from = position_;
        if (from < clip->start() || from >= clip->endExclusive()) {
            return Error{ErrorCode::InvalidData, "put the playhead over the clip first"};
        }

        const render::MaskBounds bounds = render::maskBounds(clip->maskAt(from));
        if (bounds.isEmpty()) {
            return Error{ErrorCode::InvalidData, "that mask has no area to track"};
        }

        render::RenderGraph graph{*media_};
        graph.setProject(&project_);
        graph.setTextRasterizer(&text_);
        graph.setRenderCache(&renderCache_);

        const auto rate = sequence->frameRate();
        const auto step = time::RationalTime{1, rate};
        render::RgbaImage previous;
        if (Status status = graph.compositeInto(*sequence, from, previous); !status) {
            return status.error();
        }

        // The search window scales with the frame rather than being a fixed
        // number of pixels: twenty pixels is a big move at 720p and a twitch at
        // 4K, and what somebody means by "it moves about this fast" is the
        // former. Capped, because the cost is the square of it and a window
        // wide enough to cover a whipping pan is also wide enough to find the
        // wrong lamppost.
        render::PatchWindow window;
        window.search = std::clamp(static_cast<double>(sequence->width()) * 0.02, 8.0, 48.0);

        model::Curve xs;
        model::Curve ys;
        double dx = clip->parameterAt(model::Param::MaskX, from);
        double dy = clip->parameterAt(model::Param::MaskY, from);
        // Both curves start with where the mask already is, so the frames
        // before the track are not dragged along by the first keyframe.
        xs.set(model::Keyframe{clip->sourceTimeAt(from), dx, model::Interpolation::Linear, {}, {}});
        ys.set(model::Keyframe{clip->sourceTimeAt(from), dy, model::Interpolation::Linear, {}, {}});

        MaskTrack result;
        result.confidence = 1.0;
        for (time::RationalTime at = from + step; at < clip->endExclusive(); at = at + step) {
            render::RgbaImage current;
            if (Status status = graph.compositeInto(*sequence, at, current); !status) {
                return status.error();
            }
            window.centreX = (static_cast<double>(sequence->width()) / 2.0) + bounds.centreX() + dx;
            window.centreY =
                (static_cast<double>(sequence->height()) / 2.0) + bounds.centreY() + dy;
            window.halfWidth = std::max(8.0, bounds.width() / 2.0);
            window.halfHeight = std::max(8.0, bounds.height() / 2.0);

            const render::PatchTrack moved = render::trackPatch(previous, current, window);
            if (!moved.usable) {
                result.stopped = moved.reason;
                break;
            }
            dx += moved.dx;
            dy += moved.dy;
            result.confidence = std::min(result.confidence, moved.confidence);
            ++result.frames;
            const time::RationalTime when = clip->sourceTimeAt(at);
            xs.set(model::Keyframe{when, dx, model::Interpolation::Linear, {}, {}});
            ys.set(model::Keyframe{when, dy, model::Interpolation::Linear, {}, {}});
            previous = std::move(current);
        }

        if (result.frames == 0) {
            return Error{ErrorCode::InvalidData,
                         result.stopped.empty() ? "there is nothing after this frame to track into"
                                                : result.stopped};
        }

        auto built =
            edit::makeTrackMask(project_, {sequence->id(), selectedTrack_}, selectedClip_, xs, ys);
        if (!built) {
            return built.error();
        }
        commands_.execute(project_, std::move(*built));
        commands_.breakMerge();
        renderCache_.clear();
        monitor_->update();
        timeline_->update();
        effects_->refresh();
        updateCacheBar();
        updateTitle();
        return result;
    }

    /// Hold the selected clip still.
    ///
    /// **On the clip's own frames, not on the composite.** What is being
    /// measured is how the camera moved, and the composite already has this
    /// clip's transform applied to it -- including the correction being
    /// computed, which would make the analysis chase its own tail. It also has
    /// whatever is layered over the clip in it, which moved for reasons of its
    /// own.
    Result<render::StabiliseResult> stabiliseClip() {
        const model::Sequence* sequence = liveSequence();
        const model::Track* track =
            sequence != nullptr ? sequence->findTrack(selectedTrack_) : nullptr;
        const model::Clip* clip = track != nullptr ? track->find(selectedClip_) : nullptr;
        if (clip == nullptr || media_ == nullptr) {
            return Error{ErrorCode::InvalidData, "select the clip to stabilise"};
        }
        if (clip->graphic.kind != model::GraphicKind::None || clip->nested.isValid() ||
            !clip->activeSource().isValid()) {
            return Error{ErrorCode::InvalidData,
                         "there is nothing to stabilise: this clip is generated, not filmed"};
        }

        const auto step = time::RationalTime{1, sequence->frameRate()};
        std::vector<time::RationalTime> times;
        std::vector<time::RationalTime> timeline;
        for (time::RationalTime at = clip->start(); at < clip->endExclusive(); at = at + step) {
            timeline.push_back(at);
            times.push_back(clip->activeSourceTimeAt(at));
        }

        auto analysed = render::stabilise(*media_, clip->activeSource(), times);
        if (!analysed) {
            return analysed;
        }

        model::Curve xs;
        model::Curve ys;
        for (std::size_t i = 0; i < analysed->x.size() && i < timeline.size(); ++i) {
            // Keyframes at the clip's own source times, like every other curve
            // in the project: a stabilised clip that is then trimmed or moved
            // keeps its correction glued to the frames it was measured from.
            const time::RationalTime when = clip->sourceTimeAt(timeline[i]);
            xs.set(model::Keyframe{when, analysed->x[i], model::Interpolation::Linear, {}, {}});
            ys.set(model::Keyframe{when, analysed->y[i], model::Interpolation::Linear, {}, {}});
        }
        if (xs.empty()) {
            return Error{ErrorCode::InvalidData, "there is not enough of this clip to stabilise"};
        }

        auto built = edit::makeStabilise(project_, {sequence->id(), selectedTrack_}, selectedClip_,
                                         xs, ys, analysed->zoom);
        if (!built) {
            return built.error();
        }
        commands_.execute(project_, std::move(*built));
        commands_.breakMerge();
        renderCache_.clear();
        monitor_->update();
        timeline_->update();
        effects_->refresh();
        updateCacheBar();
        updateTitle();
        return analysed;
    }

    /// Find the project's missing files under a folder and point it at them.
    ///
    /// Everything found is relinked, and what is not found is reported: a
    /// dialog per file would be a dialog per file, and the report says exactly
    /// which ones were matched only by their name.
    Result<io::RelinkReport> relinkMedia(const std::string& root) {
        auto report = io::findRelinks(project_, root);
        if (!report) {
            return report;
        }
        for (const io::RelinkMatch& match : report->matches) {
            auto built = edit::makeRelinkMedia(project_, match.media, match.found);
            if (!built) {
                continue;  // it went missing again between looking and linking
            }
            commands_.execute(project_, std::move(*built));
        }
        commands_.breakMerge();
        if (!report->matches.empty()) {
            if (Status reopened = openMedia(); !reopened) {
                return reopened.error();
            }
            renderCache_.clear();
            monitor_->update();
            timeline_->update();
            bin_->refresh();
            updateCacheBar();
            updateTitle();
        }
        return report;
    }

    /// Make a proxy for one media reference and attach it.
    ///
    /// Beside the original by default, named after it. Not in a temporary
    /// folder: a proxy the system might delete under a project is worse than
    /// no proxy, and one nobody can find is one everybody remakes.
    Result<platform::ffmpeg::ProxySummary> buildProxy(model::MediaRefId mediaId,
                                                      std::int32_t width = 960) {
        const model::MediaRef* media = project_.findMedia(mediaId);
        if (media == nullptr) {
            return Error{ErrorCode::NotFound, "no such media in this project"};
        }
        const std::filesystem::path original{media->path};
        platform::ffmpeg::ProxySettings settings;
        settings.source = media->path;
        settings.destination =
            (original.parent_path() / (original.stem().string() + "-proxy.mov")).string();
        settings.width = width;

        auto made = platform::ffmpeg::makeProxy(settings);
        if (!made) {
            return made;
        }
        for (model::MediaRef& entry : project_.mediaMutable()) {
            if (entry.id == mediaId) {
                entry.proxyPath = made->path;
            }
        }
        // Not switched on by anything here: making one and using one are
        // separate decisions, and somebody who makes proxies for a long import
        // does not necessarily want the picture to change under them now.
        bin_->refresh();
        updateTitle();
        return made;
    }

    /// Tick the comment under the playhead off, or put it back.
    ///
    /// A toggle rather than two commands, because the mistake somebody makes
    /// is ticking off the wrong one, and the fix for that has to be the same
    /// keystroke again.
    Result<bool> toggleCommentHere() {
        const model::Sequence* sequence = liveSequence();
        if (sequence == nullptr) {
            return Error{ErrorCode::InvalidData, "there is no sequence"};
        }
        const model::Marker* found = nullptr;
        for (const model::Marker& marker : sequence->markers()) {
            if (position_ >= marker.range.start() && position_ < marker.range.endExclusive()) {
                found = &marker;
            }
        }
        if (found == nullptr) {
            return Error{ErrorCode::NotFound, "there is no comment at the playhead"};
        }
        const bool resolved = !found->resolved;
        auto built = edit::makeSetMarkerReview(
            project_, sequence->id(), found->id,
            found->author.empty() ? io::thisProcess().user : found->author, resolved);
        if (!built) {
            return built.error();
        }
        commands_.execute(project_, std::move(*built));
        commands_.breakMerge();
        timeline_->update();
        updateTitle();
        return resolved;
    }

    /// Write the comments out as something to send somebody.
    Status writeReviewNotes(const std::string& path) {
        const model::Sequence* sequence = liveSequence();
        if (sequence == nullptr) {
            return Error{ErrorCode::InvalidData, "there is no sequence"};
        }
        return io::writeReviewNotes(*sequence, path);
    }

    /// Open the browser, on the folder the project's media came from.
    ///
    /// Starting there rather than at the home folder: somebody browsing for
    /// media in a project that already has some is nearly always looking in
    /// the same place they got the last lot.
    app::MediaBrowser* browseMedia() {
        if (browser_ == nullptr) {
            browser_ = new app::MediaBrowser(this);
            browser_->setProject(&project_, &commands_);
            connect(browser_, &app::MediaBrowser::imported, this, [this] {
                if (Status reopened = openMedia(); !reopened) {
                    app::warn(this, "Import", QString::fromStdString(reopened.error().toString()));
                }
                bin_->refresh();
                updateTitle();
            });
        }
        if (browser_->folder().empty()) {
            std::string start = std::filesystem::current_path().string();
            for (const model::MediaRef& media : project_.media()) {
                const std::filesystem::path parent =
                    std::filesystem::path{media.path}.parent_path();
                if (!parent.empty() && std::filesystem::is_directory(parent)) {
                    start = parent.string();
                }
            }
            static_cast<void>(browser_->showFolder(start));
        }
        browser_->show();
        browser_->raise();
        return browser_;
    }

    /// Put the keymap's current binding on one action.
    ///
    /// **Only keystrokes with a modifier become Qt shortcuts.** A bare letter
    /// bound through Qt fires while somebody is typing a title into a text
    /// field; those are dispatched from this window's key handler instead,
    /// which only sees a press when nothing else wanted it. That split used to
    /// be a hand-maintained list of "the ones that are not menu items"; it is
    /// derived now, so rebinding Save to "S" moves it to the safe path by
    /// itself.
    void applyShortcut(const QString& actionId, QAction* action) {
        const std::string shortcut = keymap_.shortcutFor(actionId.toStdString());
        const bool hasModifier = shortcut.find('+') != std::string::npos;
        if (shortcut.empty() || !hasModifier) {
            action->setShortcut(QKeySequence{});
            return;
        }
        action->setShortcut(
            QKeySequence::fromString(QString::fromStdString(shortcut), QKeySequence::PortableText));
        action->setShortcutContext(Qt::WindowShortcut);
    }

    /// Re-read every binding: what the manager calls when something changed.
    void applyKeymap() {
        for (auto entry = actions_.constBegin(); entry != actions_.constEnd(); ++entry) {
            applyShortcut(entry.key(), entry.value());
        }
    }

    /// Where a customised keymap lives.
    ///
    /// A file in the user's config folder rather than a value inside the
    /// settings blob: a keymap is a thing people share, back up, put in a
    /// dotfile repository and edit by hand when a shortcut has gone somewhere
    /// they cannot press.
    [[nodiscard]] static QString keymapPath() {
        if (!keymapPath_.isEmpty()) {
            return keymapPath_;
        }
        const QString folder = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        return folder + "/keymap.conf";
    }

    /// Put the keymap somewhere else.
    ///
    /// For the self-test, which rebinds things and must not leave them rebound
    /// in whoever ran it -- it did exactly that once, and the next run failed
    /// on a Save still sitting where the previous run had moved it. Also how
    /// somebody keeps a keymap beside a project rather than in their home
    /// directory: ZARO_KEYMAP names the file.
    static void setKeymapPath(const QString& path) { keymapPath_ = path; }

    /// Whether to interrupt. See `app::setQuiet`.
    static void setQuietMode(bool quiet) { app::setQuiet(quiet); }

    void loadKeymap() {
        QFile file{keymapPath()};
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return;  // nobody has customised anything, which is the usual case
        }
        auto loaded = ui::Keymap::decode(QString::fromUtf8(file.readAll()).toStdString());
        if (!loaded) {
            // A keymap that will not parse leaves the defaults in place rather
            // than stopping the application: somebody who hand-edited it into
            // a mess should still be able to start the program and fix it.
            std::fprintf(stderr, "zaro: %s\n", loaded.error().toString().c_str());
            return;
        }
        keymap_ = std::move(*loaded);
    }

    void saveKeymap() {
        const QString path = keymapPath();
        QDir{}.mkpath(QFileInfo{path}.absolutePath());
        QFile file{path};
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            return;
        }
        file.write(QByteArray::fromStdString(keymap_.encode()));
    }

    /// The hotkey manager.
    app::Hotkeys* showHotkeys() {
        if (hotkeys_ == nullptr) {
            hotkeys_ = new app::Hotkeys(keymap_, this);
            hotkeys_->setOnChanged([this] {
                applyKeymap();
                saveKeymap();
            });
        }
        hotkeys_->refresh();
        hotkeys_->show();
        hotkeys_->raise();
        return hotkeys_;
    }

    /// Gather the project's media into one folder and point it at the copies.
    ///
    /// The copying and the relinking are one action here and two underneath:
    /// the copy is a filesystem change nothing can undo, and the relink is an
    /// edit like any other. Undo therefore puts the project back on the
    /// originals and leaves the copies where they are, which is the honest
    /// half to be able to take back.
    Result<io::ConsolidateReport> consolidateMedia(const std::string& destination) {
        auto report = io::consolidate(project_, destination);
        if (!report) {
            return report;
        }
        for (const io::ConsolidatedFile& file : report->files) {
            if (file.alreadyThere) {
                continue;
            }
            auto built = edit::makeRelinkMedia(project_, file.media, file.to);
            if (!built) {
                continue;
            }
            commands_.execute(project_, std::move(*built));
        }
        commands_.breakMerge();
        if (Status reopened = openMedia(); !reopened) {
            return reopened.error();
        }
        renderCache_.clear();
        monitor_->update();
        timeline_->update();
        bin_->refresh();
        updateCacheBar();
        updateTitle();
        return report;
    }

    /// Show the transcript, and edit by it.
    app::Transcript* showTranscript() {
        if (transcript_ == nullptr) {
            transcript_ = new app::Transcript(this);
            connect(transcript_, &app::Transcript::edited, this, [this] {
                renderCache_.clear();
                timeline_->update();
                monitor_->update();
                updateCacheBar();
                updateTitle();
                refresh();
            });
            connect(transcript_, &app::Transcript::scrubbed, this,
                    [this](time::RationalTime at) { setPosition(at); });
        }
        transcript_->setProject(&project_, sequenceId_, &commands_);
        transcript_->show();
        transcript_->raise();
        return transcript_;
    }

    /// Fit the selected music clip to a length by taking a piece out of it.
    ///
    /// The length wanted is the picture's: fitting music to a cut is the
    /// errand, and asking for a number when the answer is on screen would be a
    /// question with one sensible reply.
    Result<render::RemixPlan> remixSelectedTo(double targetSeconds) {
        const model::Sequence* sequence = liveSequence();
        const model::Track* track =
            sequence != nullptr ? sequence->findTrack(selectedTrack_) : nullptr;
        const model::Clip* clip = track != nullptr ? track->find(selectedClip_) : nullptr;
        if (clip == nullptr || media_ == nullptr) {
            return Error{ErrorCode::InvalidData, "select the music to fit"};
        }
        if (!clip->activeSource().isValid()) {
            return Error{ErrorCode::InvalidData, "that clip has no media to look at"};
        }
        const model::MediaRef* media = project_.findMedia(clip->activeSource());
        if (media == nullptr || media->info.primaryAudio() == nullptr) {
            return Error{ErrorCode::InvalidData, "there is no sound in that clip"};
        }

        const double have = clip->sourceRange.duration().toSecondsDouble();
        auto beats =
            render::detectBeats(*media_, clip->activeSource(), have, sequence->audioSampleRate());
        if (!beats) {
            return beats.error();
        }
        // The beats are measured from the clip's own start, so a clip already
        // trimmed into the middle of a track still cuts on its own beats.
        auto plan = render::planRemix(*beats, have, targetSeconds);
        if (!plan) {
            return plan;
        }

        auto built = edit::makeRemix(project_, {sequence->id(), selectedTrack_}, selectedClip_,
                                     plan->cutAt, plan->resumeFrom, plan->joinFade);
        if (!built) {
            return built.error();
        }
        commands_.execute(project_, std::move(*built));
        commands_.breakMerge();
        renderCache_.clear();
        timeline_->update();
        monitor_->update();
        updateCacheBar();
        updateTitle();
        return plan;
    }

    /// Recompose the selected clip to fill the sequence's frame.
    ///
    /// **On the clip's own frames**, like the stabiliser and for the same
    /// reason: the composite already has this clip's transform on it, and the
    /// transform is what is being decided.
    Result<render::ReframeResult> reframeClip() {
        const model::Sequence* sequence = liveSequence();
        const model::Track* track =
            sequence != nullptr ? sequence->findTrack(selectedTrack_) : nullptr;
        const model::Clip* clip = track != nullptr ? track->find(selectedClip_) : nullptr;
        if (clip == nullptr || media_ == nullptr) {
            return Error{ErrorCode::InvalidData, "select the clip to reframe"};
        }
        if (clip->graphic.kind != model::GraphicKind::None || clip->nested.isValid() ||
            !clip->activeSource().isValid()) {
            return Error{ErrorCode::InvalidData,
                         "there is nothing to reframe: this clip is generated, not filmed"};
        }

        const auto step = time::RationalTime{1, sequence->frameRate()};
        std::vector<time::RationalTime> times;
        std::vector<time::RationalTime> timeline;
        for (time::RationalTime at = clip->start(); at < clip->endExclusive(); at = at + step) {
            timeline.push_back(at);
            times.push_back(clip->activeSourceTimeAt(at));
        }

        auto framed = render::autoReframe(*media_, clip->activeSource(), times, sequence->width(),
                                          sequence->height());
        if (!framed) {
            return framed;
        }

        model::Curve xs;
        model::Curve ys;
        for (std::size_t i = 0; i < framed->x.size() && i < timeline.size(); ++i) {
            const time::RationalTime when = clip->sourceTimeAt(timeline[i]);
            xs.set(model::Keyframe{when, framed->x[i], model::Interpolation::Linear, {}, {}});
            ys.set(model::Keyframe{when, framed->y[i], model::Interpolation::Linear, {}, {}});
        }
        if (xs.empty()) {
            return Error{ErrorCode::InvalidData, "there is not enough of this clip to reframe"};
        }

        auto built = edit::makeReframe(project_, {sequence->id(), selectedTrack_}, selectedClip_,
                                       xs, ys, framed->scale);
        if (!built) {
            return built.error();
        }
        commands_.execute(project_, std::move(*built));
        commands_.breakMerge();
        renderCache_.clear();
        monitor_->update();
        timeline_->update();
        effects_->refresh();
        updateCacheBar();
        updateTitle();
        return framed;
    }

    /// Pin the selected clip to one on a lower track, or to nothing.
    Result<model::ClipId> pinTo(model::ClipId host) {
        const model::Sequence* sequence = liveSequence();
        if (sequence == nullptr || !selectedClip_.isValid()) {
            return Error{ErrorCode::InvalidData, "select the clip to pin first"};
        }
        auto built =
            edit::makePinTo(project_, {sequence->id(), selectedTrack_}, selectedClip_, host);
        if (!built) {
            return built.error();
        }
        commands_.execute(project_, std::move(*built));
        commands_.breakMerge();
        renderCache_.clear();
        monitor_->update();
        timeline_->update();
        effects_->refresh();
        updateCacheBar();
        updateTitle();
        return host;
    }

    /// Pin to whatever is underneath at the playhead.
    ///
    /// The topmost clip on a lower track, because that is the one somebody can
    /// see: pinning to something hidden behind another picture would be
    /// pinning to a thing that is not there as far as they are concerned.
    Result<model::ClipId> pinToClipBelow() {
        const model::Sequence* sequence = liveSequence();
        if (sequence == nullptr || !selectedClip_.isValid()) {
            return Error{ErrorCode::InvalidData, "select the clip to pin first"};
        }
        const model::Clip* found = nullptr;
        for (const model::Track& track : sequence->videoTracks()) {
            if (track.id() == selectedTrack_) {
                break;  // tracks are listed bottom-up, so this is where "below" ends
            }
            if (!sequence->isAudible(track)) {
                continue;
            }
            if (const model::Clip* candidate = track.clipAt(position_)) {
                found = candidate;
            }
        }
        if (found == nullptr) {
            return Error{ErrorCode::InvalidData, "there is nothing under this clip to pin it to"};
        }
        return pinTo(found->id);
    }

    /// Save the selected graphic on its own, so it can be used again.
    Status saveGraphicTemplate(const std::string& path) {
        const model::Sequence* sequence = liveSequence();
        const model::Track* track =
            sequence != nullptr ? sequence->findTrack(selectedTrack_) : nullptr;
        const model::Clip* clip = track != nullptr ? track->find(selectedClip_) : nullptr;
        if (clip == nullptr) {
            return Error{ErrorCode::InvalidData, "select the title or shape to save"};
        }
        return io::saveGraphicTemplate(*clip, path);
    }

    /// Drop a saved graphic in at the playhead, on the selected track.
    Result<model::ClipId> placeGraphicTemplate(const std::string& path,
                                               const time::RationalTime& duration) {
        const model::Sequence* sequence = liveSequence();
        if (sequence == nullptr) {
            return Error{ErrorCode::InvalidData, "there is no sequence to place it in"};
        }
        const model::TrackId trackId =
            selectedTrack_.isValid() ? selectedTrack_ : sequence->videoTracks().front().id();
        auto loaded = io::loadGraphicTemplate(path);
        if (!loaded) {
            return loaded.error();
        }
        // As long as it was designed to be, unless somebody says otherwise: a
        // template dropped in at some arbitrary length is a template whose
        // timing nobody chose.
        time::RationalTime length = duration;
        if (length.toSecondsDouble() <= 0.0) {
            length = loaded->responsive.authored.toSecondsDouble() > 0.0
                         ? loaded->responsive.authored
                         : loaded->sourceRange.duration();
        }
        const time::TimeRange range{position_, length.rescaledTo(position_.rate())};
        auto built =
            edit::makePlaceGraphicTemplate(project_, {sequence->id(), trackId}, *loaded, range);
        if (!built) {
            return built.error();
        }
        commands_.execute(project_, std::move(*built));
        commands_.breakMerge();
        renderCache_.clear();
        monitor_->update();
        timeline_->update();
        updateCacheBar();
        updateTitle();

        const model::Track* track = project_.findSequence(sequence->id())->findTrack(trackId);
        for (const model::Clip& candidate : track->clips()) {
            if (candidate.start() == range.start()) {
                return candidate.id;
            }
        }
        return Error{ErrorCode::InvalidData, "the template did not land anywhere"};
    }

    /// Hold a frame and show the current one against it.
    void setComparing(bool on, const time::RationalTime& reference) {
        comparing_ = on;
        referenceAt_ = reference;
        monitor_->setComparison(on, referenceAt_, compareMode_, compareSplit_);
        monitor_->update();
    }
    [[nodiscard]] bool comparing() const noexcept { return comparing_; }

    void setCompareMode(render::CompareMode mode) {
        compareMode_ = mode;
        monitor_->setComparison(comparing_, referenceAt_, compareMode_, compareSplit_);
        monitor_->update();
    }
    void setCompareSplit(double split) {
        compareSplit_ = std::clamp(split, 0.0, 1.0);
        monitor_->setComparison(comparing_, referenceAt_, compareMode_, compareSplit_);
        monitor_->update();
    }

    /// What this sequence is delivered as, and how its highlights get there.
    ///
    /// A sequence property rather than an export option, because the curve
    /// editor and the scopes are drawn against it: choosing it at export time
    /// would mean grading against one curve and delivering through another.
    void deliveryMenu() {
        const model::Sequence* sequence = liveSequence();
        if (sequence == nullptr) {
            return;
        }
        const model::Sequence::Output current = sequence->output();

        QMenu menu;
        menu.addAction("Delivered through")->setEnabled(false);
        std::map<QAction*, media::TransferFunction> curves;
        for (const media::TransferFunction transfer : media::allTransferFunctions()) {
            if (transfer == media::TransferFunction::Unknown) {
                continue;  // no formula, so nothing could encode through it
            }
            QAction* action = menu.addAction(QString::fromUtf8(media::toString(transfer)));
            action->setCheckable(true);
            action->setChecked(current.transfer == transfer);
            curves.emplace(action, transfer);
        }

        menu.addSeparator();
        menu.addAction("Highlights")->setEnabled(false);
        struct Knee {
            const char* name;
            double value;
        };
        static constexpr Knee kKnees[] = {
            {"clip (as delivered before)", 1.0},
            {"roll off gently", 0.9},
            {"roll off", 0.8},
            {"roll off hard", 0.65},
        };
        std::map<QAction*, double> knees;
        for (const Knee& knee : kKnees) {
            QAction* action = menu.addAction(QString::fromUtf8(knee.name));
            action->setCheckable(true);
            action->setChecked(current.highlightKnee == knee.value);
            knees.emplace(action, knee.value);
        }

        QAction* chosen = menu.exec(QCursor::pos());
        if (chosen == nullptr) {
            return;
        }
        model::Sequence::Output wanted = current;
        if (const auto curve = curves.find(chosen); curve != curves.end()) {
            wanted.transfer = curve->second;
        } else if (const auto knee = knees.find(chosen); knee != knees.end()) {
            wanted.highlightKnee = knee->second;
        } else {
            return;
        }
        setDelivery(wanted);
    }

    /// The work behind the menu, separated so it can be driven without one.
    bool setDelivery(const model::Sequence::Output& output) {
        const model::Sequence* sequence = liveSequence();
        if (sequence == nullptr) {
            return false;
        }
        auto built = edit::makeSetSequenceOutput(project_, sequence->id(), output);
        if (!built) {
            app::warn(this, "Delivery", QString::fromStdString(built.error().toString()));
            return false;
        }
        commands_.execute(project_, std::move(*built));
        commands_.breakMerge();
        // The delivery curve is what curves, secondaries and LUTs are baked
        // against, so every cached frame was made for the old one.
        renderCache_.clear();
        monitor_->update();
        updateCacheBar();
        updateTitle();
        return true;
    }

    /// Cut the selected clip where the picture changes.
    ///
    /// Returns how many cuts were made, so the self-test can say what happened
    /// without a dialog. Zero is a perfectly good answer: a single continuous
    /// take has no scene changes in it, and reporting one would be worse than
    /// reporting none.
    std::int32_t detectScenes() {
        const model::Sequence* sequence = liveSequence();
        const model::Track* track =
            sequence != nullptr ? sequence->findTrack(selectedTrack_) : nullptr;
        const model::Clip* clip = track != nullptr ? track->find(selectedClip_) : nullptr;
        if (clip == nullptr || media_ == nullptr || clip->nested.isValid() ||
            clip->graphic.isSet()) {
            return 0;
        }

        const time::Rational rate = sequence->frameRate();
        const std::int64_t first = clip->start().rescaledTo(rate).frames();
        const std::int64_t count = clip->timelineRange.duration().rescaledTo(rate).frames();
        if (count <= 1) {
            return 0;
        }

        render::SceneDetectOptions options;
        // Half a second, at whatever rate this sequence runs. Expressed in time
        // rather than frames so the same setting means the same thing on a
        // 24fps cut and a 60fps one.
        options.minimumShot = time::RationalTime::fromSeconds(time::Rational{1, 2}, rate);

        QProgressDialog progress("Looking for scene changes…", "Cancel", 0, static_cast<int>(count),
                                 this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(400);

        render::SceneDetector detector{options};
        for (std::int64_t i = 0; i < count; ++i) {
            progress.setValue(static_cast<int>(i));
            QCoreApplication::processEvents();
            if (progress.wasCanceled()) {
                return 0;
            }
            const time::RationalTime at{first + i, rate};
            auto image = media_->imageFor(clip->activeSource(), clip->activeSourceTimeAt(at));
            if (!image) {
                // A frame that will not decode is a gap in the evidence, not a
                // scene change. Skipped, and the frame before it stays the one
                // the next is compared against.
                continue;
            }
            detector.push(**image, at);
        }
        detector.flush();
        progress.reset();

        std::vector<time::RationalTime> points;
        points.reserve(detector.cuts().size());
        for (const render::SceneCut& cut : detector.cuts()) {
            points.push_back(cut.at);
        }
        if (points.empty()) {
            return 0;
        }

        auto built = edit::makeRazorAt(project_, {sequence->id(), selectedTrack_}, points);
        if (!built) {
            return 0;
        }
        commands_.execute(project_, std::move(*built));
        commands_.breakMerge();
        timeline_->update();
        monitor_->update();
        updateCacheBar();
        updateTitle();
        return static_cast<std::int32_t>(points.size());
    }

    /// Open a different project into this window.
    ///
    /// Everything is loaded and the media opened *before* anything is replaced,
    /// so a file that cannot be read leaves the window on the project it
    /// already had. Half-swapping a window is how a program ends up showing one
    /// project's timeline over another's media.
    /// Reports rather than reports *and* decides how to tell somebody. The
    /// button below puts the message on screen; a caller with no screen -- the
    /// self-test -- gets the same answer without a dialog it cannot dismiss.
    /// Whether this build takes and honours project locks.
    ///
    /// **Off by default, for now.** The locking is finished and tested, but it
    /// interrupts an ordinary run -- a lock left behind by a killed process, or
    /// one written by a test, produces a dialog somebody has to answer before
    /// they can carry on. Until there is a way to clear a lock from the
    /// interface rather than from the filesystem, the interruption costs more
    /// than the protection is worth. `ZARO_LOCKING=1` turns it back on, and
    /// the self-test turns it on for the block that checks it.
    static bool lockingEnabled() { return locking_; }
    static void setLockingEnabled(bool enabled) { locking_ = enabled; }

    /// How to treat a project somebody else already has open.
    enum class Sharing : std::uint8_t {
        /// Take the lock if it is free or stale; refuse to open otherwise.
        Exclusive,
        /// Open it anyway, without saving over their work.
        ReadOnly,
        /// Their lock is stale or they have gone home: take it.
        TakeOver,
    };

    [[nodiscard]] Status openProject(const std::string& path,
                                     Sharing sharing = Sharing::Exclusive) {
        auto loaded = io::loadProject(path);
        if (!loaded) {
            return loaded.error();
        }
        if (loaded->project.findSequence(loaded->project.activeSequence()) == nullptr) {
            return Error{ErrorCode::InvalidData, "that project has no active sequence"};
        }

        // Whether anybody else is in it, decided before anything is replaced:
        // refusing after the window has already changed would be worse than
        // not opening at all.
        bool readOnly = false;
        if (auto held = io::readLock(path); lockingEnabled() && held && !io::isOurs(*held)) {
            const bool free = io::isStale(*held);
            if (!free && sharing == Sharing::Exclusive) {
                return Error{ErrorCode::InvalidData,
                             held->user + " has this project open on " + held->host};
            }
            readOnly = !free && sharing == Sharing::ReadOnly;
        }

        releaseLock();
        adopt(std::move(loaded->project), std::move(*loaded), path);
        readOnly_ = readOnly;
        if (lockingEnabled() && !readOnly_) {
            // Advisory, so a volume that will not take one is not a reason to
            // refuse to work: the failure is ignored on purpose.
            static_cast<void>(io::writeLock(path, io::thisProcess()));
        }
        updateTitle();
        return {};
    }

    /// Whether this window may write over the project it has open.
    [[nodiscard]] bool isReadOnly() const noexcept { return readOnly_; }

    /// Who else has this project, if anybody. Empty when it is ours or free.
    [[nodiscard]] std::string heldBy() const {
        if (path_.empty()) {
            return {};
        }
        auto held = io::readLock(path_);
        if (!held || io::isOurs(*held) || io::isStale(*held)) {
            return {};
        }
        return held->user + " on " + held->host;
    }

    void releaseLock() {
        if (!path_.empty() && !readOnly_) {
            static_cast<void>(io::removeLock(path_));
        }
    }

    /// Start again, with somewhere to put something.
    void newProject() {
        model::Project fresh = model::newProject();
        adopt(std::move(fresh), io::LoadedProject{}, {});
    }

    /// Replace what this window is showing.
    ///
    /// The same no-dialog bargain closing makes: whatever was unsaved is
    /// written to its recovery file first, so switching projects never asks a
    /// question and never loses anything.
    void adopt(model::Project project, io::LoadedProject loaded, std::string path) {
        autosave();
        stop();

        project_ = std::move(project);
        loaded_ = std::move(loaded);
        path_ = std::move(path);
        // Read-only is a fact about the file that has just gone, not about the
        // window. Left set, it would follow somebody into a new project and
        // refuse to save it, naming a person who has nothing to do with it.
        readOnly_ = false;
        sequenceId_ = project_.activeSequence();
        position_ = time::RationalTime{0, project_.findSequence(sequenceId_)->frameRate()};
        selectedTrack_ = model::TrackId{};
        selectedClip_ = model::ClipId{};

        // The history and the cache belong to the project that has just gone.
        // A frame cached from it would be served for the new one -- the recipe
        // covers what is in a sequence, not which project it came from.
        commands_.clear();
        commands_.markSaved();
        renderCache_.clear();

        if (Status opened = openMedia(); !opened) {
            // The project is loaded and the window is bound to it; what failed
            // is reading its files. Said plainly rather than swallowed: a
            // timeline of clips that draw nothing is a puzzle.
            app::warn(this, "Open", QString::fromStdString(opened.error().toString()));
        }
        effects_->setSelection(model::TrackId{}, model::ClipId{});
        timeline_->setCachedSpans({});
        updateTitle();
        refresh();
        monitor_->update();
    }

    /// Write the project back where it came from.
    ///
    /// Returns false when it could not be written, so a caller that was about
    /// to do something irreversible knows not to.
    bool save() {
        if (path_.empty()) {
            return saveAs();
        }
        if (readOnly_) {
            // Said on stderr and in the title bar, never in a dialog.
            //
            // This one was the worst offender: a project with a stale lock
            // beside it -- left by a killed process, or by a test -- turned
            // every Ctrl+S into a box somebody had to dismiss before they could
            // carry on. The window title already carries "[read only]", which
            // is where a state belongs; a refusal does not also need to
            // interrupt.
            std::fprintf(stderr,
                         "zaro: read only: %s has this project open; save a new "
                         "version to keep your work\n",
                         heldBy().empty() ? "somebody else" : heldBy().c_str());
            return false;
        }
        if (Status written = io::saveProject(project_, path_, loaded_.unknown); !written) {
            app::warn(this, "Save", QString::fromStdString(written.error().toString()));
            return false;
        }
        commands_.markSaved();
        // The recovery file describes work that is now in the project itself.
        // Left behind, it would be offered on the next open as though it were
        // newer, which is an alarming thing to be asked about a file that is
        // already correct.
        std::error_code code;
        std::filesystem::remove(io::autosavePath(path_), code);
        updateTitle();
        return true;
    }

    /// Save as the next version beside this one, and carry on in it.
    ///
    /// Carrying on in the new file rather than staying in the old one is the
    /// point: a version is a line somebody draws under what they had, and the
    /// next hour's work belongs after the line. The previous file is left
    /// exactly as it was, which is the other half of the point.
    Result<std::string> saveNewVersion() {
        if (path_.empty()) {
            // Nowhere to count from. Asking where to put it is the honest
            // answer, and after that there is a version one to count from.
            return Error{ErrorCode::InvalidData, "save this project once before versioning it"};
        }
        const std::string next = io::nextVersionPath(path_);
        if (Status written = io::saveProject(project_, next, loaded_.unknown); !written) {
            return written.error();
        }
        setProjectPath(next);
        // A new version is a different file, which nobody else has open.
        readOnly_ = false;
        commands_.markSaved();
        std::error_code code;
        std::filesystem::remove(io::autosavePath(next), code);
        updateTitle();
        return next;
    }

    /// The versions beside this project, to jump between.
    void openVersionMenu() {
        if (path_.empty()) {
            app::say(this, "Version", "This project has not been saved yet.");
            return;
        }
        QMenu menu;
        std::map<QAction*, std::string> paths;
        for (const std::string& version : io::versionsOf(path_)) {
            const bool current = version == path_;
            QAction* action = menu.addAction(
                QString::fromStdString(std::filesystem::path{version}.filename().string()));
            action->setCheckable(true);
            action->setChecked(current);
            action->setEnabled(!current);
            paths.emplace(action, version);
        }
        if (paths.empty()) {
            menu.addAction("No other versions")->setEnabled(false);
        }
        QAction* chosen = menu.exec(QCursor::pos());
        const auto found = paths.find(chosen);
        if (found == paths.end()) {
            return;
        }
        if (Status opened = openProject(found->second); !opened) {
            app::warn(this, "Open", QString::fromStdString(opened.error().toString()));
        }
    }

    void openDialog() {
        const QString chosen =
            QFileDialog::getOpenFileName(this, "Open project", {}, "Zaro projects (*.zaro)");
        if (chosen.isEmpty()) {
            return;
        }
        const std::string path = chosen.toStdString();

        // Somebody else's lock is a question, not a refusal: often enough the
        // answer is "let me look at it anyway", and often enough the other
        // machine went home hours ago.
        if (auto held = io::readLock(path); held && !io::isOurs(*held) && !io::isStale(*held)) {
            QMessageBox ask{this};
            ask.setWindowTitle("Open project");
            ask.setText(
                QString("%1 has this project open on %2.")
                    .arg(QString::fromStdString(held->user), QString::fromStdString(held->host)));
            ask.setInformativeText(
                "Opening it read-only lets you look without saving over "
                "their work.");
            QPushButton* readOnly = ask.addButton("Open Read-Only", QMessageBox::AcceptRole);
            QPushButton* takeOver = ask.addButton("Take Over", QMessageBox::DestructiveRole);
            ask.addButton(QMessageBox::Cancel);
            ask.exec();

            Sharing sharing = Sharing::Exclusive;
            if (ask.clickedButton() == readOnly) {
                sharing = Sharing::ReadOnly;
            } else if (ask.clickedButton() == takeOver) {
                sharing = Sharing::TakeOver;
            } else {
                return;
            }
            if (Status opened = openProject(path, sharing); !opened) {
                app::warn(this, "Open", QString::fromStdString(opened.error().toString()));
            }
            return;
        }

        if (Status opened = openProject(path); !opened) {
            app::warn(this, "Open", QString::fromStdString(opened.error().toString()));
        }
    }

    bool saveAs() {
        const QString chosen = QFileDialog::getSaveFileName(
            this, "Save project", QString::fromStdString(path_.empty() ? "project.zaro" : path_),
            "Zaro projects (*.zaro)");
        if (chosen.isEmpty()) {
            return false;
        }
        // Saving somewhere else is exactly the way out of somebody else's
        // lock, so it clears read-only rather than being refused by it -- the
        // old advice was "save a new version to keep your work", which the
        // program then would not let anybody do.
        setProjectPath(chosen.toStdString());
        readOnly_ = false;
        return save();
    }

    /// Write the recovery file, if there is anything to recover.
    void autosave() {
        if (path_.empty() || !commands_.isModified()) {
            return;
        }
        // Failures are silent on purpose. An autosave is something the program
        // does on its own, and a dialog interrupting somebody mid-edit to
        // report it is worse than the missing file -- the next explicit save
        // will report the same problem at a moment they are expecting an
        // answer.
        static_cast<void>(io::saveProject(project_, io::autosavePath(path_), loaded_.unknown));
    }

    [[nodiscard]] const std::string& projectPath() const noexcept { return path_; }

    /// Point Save at a different file. What Save As does once somebody has
    /// chosen one.
    void setProjectPath(std::string path) {
        path_ = std::move(path);
        updateTitle();
    }

    /// The file name, and whether it differs from what is on disk.
    void updateTitle() {
        const QString name = path_.empty() ? QString{"Untitled"}
                                           : QFileInfo(QString::fromStdString(path_)).fileName();
        // Said in the title, because read-only is a fact about the whole
        // window and finding out at the moment of saving is finding out too
        // late.
        setWindowTitle(
            QString("%1%2%3 — Zaro")
                .arg(name, commands_.isModified() ? "*" : "", readOnly_ ? " [read only]" : ""));
        if (statusLeft_ != nullptr) {
            updateChrome();
        }
    }

    /// Open the source monitor on the frame the selected clip is showing.
    void matchFrame() {
        const model::Sequence* sequence = liveSequence();
        const model::Track* track =
            sequence != nullptr ? sequence->findTrack(selectedTrack_) : nullptr;
        const model::Clip* clip = track != nullptr ? track->find(selectedClip_) : nullptr;
        if (clip == nullptr ||
            !clip->timelineRange.contains(position_.rescaledTo(clip->start().rate()))) {
            return;
        }
        const model::MediaRef* ref = project_.findMedia(clip->activeSource());
        if (ref == nullptr) {
            // A generated clip or a nest has no frame of a file to match to.
            return;
        }
        source_->showFrame(*ref, clip->activeSourceTimeAt(position_));
        setSourceShown(true);
    }

    /// Keep what is marked in the source monitor as a subclip.
    void makeSubclip() {
        const auto range = source_->markedRange();
        if (!range || !source_->media().isValid()) {
            return;
        }
        const model::MediaRef* ref = project_.findMedia(source_->media());
        if (ref == nullptr) {
            return;
        }
        model::Subclip subclip;
        subclip.id = project_.ids().next<model::SubclipTag>();
        subclip.source = ref->id;
        subclip.range = *range;
        // Numbered rather than asked for. Naming every subclip at the moment it
        // is made is a dialog between somebody and the thing they were doing;
        // the bin lists them under the file they came from, which is how they
        // are found anyway.
        std::size_t existing = 0;
        for (const model::Subclip& other : project_.subclips()) {
            existing += other.source == ref->id ? 1 : 0;
        }
        subclip.name = ref->name + " [" + std::to_string(existing + 1) + "]";
        project_.addSubclip(std::move(subclip));
        bin_->refresh();
    }

    /// Point the selected timeline clip at different media.
    void replaceSelectedSource(model::MediaRefId media) {
        const model::Sequence* sequence = liveSequence();
        if (sequence == nullptr || !selectedClip_.isValid()) {
            return;
        }
        auto built = edit::makeReplaceSource(project_, {sequence->id(), selectedTrack_},
                                             selectedClip_, media);
        if (!built) {
            app::warn(this, "Replace footage", QString::fromStdString(built.error().toString()));
            return;
        }
        commands_.execute(project_, std::move(*built));
        commands_.breakMerge();
        monitor_->update();
        timeline_->update();
        updateCacheBar();
    }

    /// The work behind the menu, separated so it can be driven without one.
    ///
    /// Reports what it could not do rather than silently doing less: a camera
    /// left unsynced is a cut that lands wrong later, and the moment to find
    /// out is now.
    void syncAngles(bool byEar) {
        const model::Sequence* sequence = liveSequence();
        if (sequence == nullptr) {
            return;
        }
        const model::Track* track = sequence->findTrack(selectedTrack_);
        const model::Clip* clip = track != nullptr ? track->find(selectedClip_) : nullptr;
        if (clip == nullptr || !clip->isMulticam()) {
            return;
        }

        auto synced = byEar ? edit::syncByAudio(project_, *clip, *media_)
                            : edit::syncByTimecode(project_, *clip);
        if (!synced) {
            app::warn(this, "Multicam", QString::fromStdString(synced.error().toString()));
            return;
        }

        std::vector<std::pair<std::int32_t, time::RationalTime>> offsets;
        QStringList skipped;
        for (const edit::AngleSync& entry : *synced) {
            if (entry.offset.has_value()) {
                offsets.emplace_back(entry.angle, *entry.offset);
                continue;
            }
            const std::string& name = clip->angles[static_cast<std::size_t>(entry.angle)].name;
            skipped.append(QString("%1: %2")
                               .arg(QString::fromStdString(name))
                               .arg(QString::fromStdString(entry.reason)));
        }

        if (!offsets.empty()) {
            auto built = edit::makeSetAngleOffsets(project_, {sequence->id(), selectedTrack_},
                                                   selectedClip_, offsets);
            if (!built) {
                app::warn(this, "Multicam", QString::fromStdString(built.error().toString()));
                return;
            }
            commands_.execute(project_, std::move(*built));
            commands_.breakMerge();
            monitor_->update();
            timeline_->update();
            updateCacheBar();
        }
        lastSyncCount_ = static_cast<std::int32_t>(offsets.size());
        lastSyncSkipped_ = static_cast<std::int32_t>(skipped.size());

        if (!skipped.isEmpty()) {
            app::say(this, "Multicam",
                     QString("Synced %1 of %2 angles.\n\nNot synced:\n%3")
                         .arg(offsets.size())
                         .arg(clip->angles.size())
                         .arg(skipped.join("\n")));
        }
    }

    [[nodiscard]] std::int32_t lastSyncCount() const noexcept { return lastSyncCount_; }
    [[nodiscard]] std::int32_t lastSyncSkipped() const noexcept { return lastSyncSkipped_; }

    /// Recompute what the cache bar shows.
    ///
    /// Sampled at one point per pixel of the timeline's width: the bar cannot
    /// show more than that, and checking every frame of a long sequence would
    /// hash the whole timeline on every repaint.
    void updateCacheBar() {
        const model::Sequence* sequence = liveSequence();
        if (sequence == nullptr) {
            return;
        }
        const time::TimeRange visible = timeline_->layout().visibleRange(sequence->frameRate());
        timeline_->setCachedSpans(
            render::cachedSpans(renderCache_, &project_, *sequence, visible, timeline_->width()));
    }

    /// The work behind the menu entry, separated from the menu so that it can
    /// be driven without one.
    void renderVisibleRange() {
        const model::Sequence* sequence = liveSequence();
        if (sequence == nullptr || media_ == nullptr) {
            return;
        }
        const time::TimeRange visible = timeline_->layout().visibleRange(sequence->frameRate());
        if (visible.isEmpty()) {
            return;
        }

        QProgressDialog progress("Rendering…", "Cancel", 0,
                                 static_cast<int>(visible.duration().frames()), this);
        progress.setWindowModality(Qt::WindowModal);
        // A pre-render of a few frames is not worth a dialog appearing and
        // vanishing; one of a few hundred is.
        progress.setMinimumDuration(400);

        render::RenderGraph graph{*media_};
        graph.setProject(&project_);
        graph.setTextRasterizer(&text_);
        graph.setRenderCache(&renderCache_);
        auto stats = render::prerender(graph, renderCache_, &project_, *sequence, visible,
                                       [&progress](std::int32_t done, std::int32_t total) {
                                           progress.setMaximum(total);
                                           progress.setValue(done);
                                           QCoreApplication::processEvents();
                                           return !progress.wasCanceled();
                                       });
        progress.reset();
        if (!stats) {
            app::warn(this, "Render", QString::fromStdString(stats.error().toString()));
            return;
        }
        updateCacheBar();
    }

    Status openMedia() {
        auto opened = platform::ffmpeg::ProjectMediaSource::open(project_);
        if (!opened) {
            return opened.error();
        }
        media_ = std::move(*opened);
        monitor_->setSource(liveSequence(), media_.get());
        monitor_->setTextRasterizer(&text_);
        monitor_->setNesting(&project_, media_.get());
        monitor_->setRenderCache(&renderCache_);
        timeline_->setProject(&project_, liveSequence()->id(), &commands_);
        effects_->setProject(&project_, liveSequence()->id(), &commands_);
        effects_->setAudioSource(media_.get());
        mixer_->setProject(&project_, liveSequence()->id(), &commands_);
        bin_->setProject(&project_, liveSequence()->id(), &commands_);
        source_->setProvider(media_.get());
        startWaveforms();
        scrubber_->setRange(0, static_cast<int>(liveSequence()->duration().frames()));
        refresh();
        return {};
    }

    void setPosition(const time::RationalTime& position) {
        const std::int64_t last =
            std::max<std::int64_t>(0, liveSequence()->duration().frames() - 1);
        const std::int64_t clamped = std::clamp<std::int64_t>(position.frames(), 0, last);
        position_ = time::RationalTime{clamped, liveSequence()->frameRate()};
        monitor_->setPosition(position_);
        timeline_->setPlayhead(position_);
        // The panel shows values at the playhead and writes keyframes there, so
        // it has to know where the playhead is.
        effects_->setPosition(position_);
        refreshInstruments();
        refresh();
    }

    void step(std::int64_t frames) {
        stop();
        setPosition(position_ + time::RationalTime{frames, liveSequence()->frameRate()});
    }

protected:
    /// Turn a key press into the same text a keymap holds.
    ///
    /// Qt's own portable spelling, run through the keymap's normaliser, so
    /// there is one form and one comparison rather than a second opinion about
    /// what "Shift+Left" is called.
    [[nodiscard]] static std::string keystrokeOf(const QKeyEvent* event) {
        switch (event->key()) {
            case Qt::Key_Control:
            case Qt::Key_Shift:
            case Qt::Key_Alt:
            case Qt::Key_Meta:
                return {};  // a modifier on its own is not a keystroke
            default:
                break;
        }
        const QKeySequence sequence{event->keyCombination()};
        auto normalised =
            ui::normaliseShortcut(sequence.toString(QKeySequence::PortableText).toStdString());
        return normalised ? *normalised : std::string{};
    }

    void keyPressEvent(QKeyEvent* event) override {
        // Whatever the keymap says this keystroke is, whether or not it has a
        // menu item behind it. There used to be a table here of "the bindings
        // that are not menu items" and a switch for playback underneath it,
        // which meant three places knew what a key did and only one of them
        // could be changed. Now there is one: the keymap.
        const std::string keystroke = keystrokeOf(event);
        const std::string wanted = keystroke.empty() ? std::string{} : keymap_.actionFor(keystroke);
        if (!wanted.empty() && trigger(wanted)) {
            return;
        }
        QWidget::keyPressEvent(event);
    }

public:
    /// Run an action by name.
    ///
    /// The single door everything goes through -- a menu item, a key press, a
    /// test -- so "what does this command do" has one answer and a self-test
    /// can press a button that has no button.
    bool trigger(const std::string& actionId) {
        if (QAction* action = actions_.value(QString::fromStdString(actionId), nullptr)) {
            action->trigger();
            return true;
        }
        const auto handler = handlers_.find(actionId);
        if (handler == handlers_.end()) {
            return false;
        }
        handler->second();
        return true;
    }

    /// Register something that has no menu item: playback, marking, stepping.
    ///
    /// These are the commands whose defaults are bare letters, which cannot be
    /// Qt shortcuts without firing while somebody types. They are actions like
    /// any other -- catalogued, rebindable, listed in the manager -- they
    /// simply arrive through the key handler rather than through a menu.
    template <typename F>
    void bindAction(const char* actionId, F&& handler) {
        ZARO_CHECK(ui::findAction(actionId) != nullptr,
                   "a binding names an action nobody catalogued");
        handlers_.emplace(actionId, std::forward<F>(handler));
    }

    [[nodiscard]] ui::Keymap& keymap() { return keymap_; }

protected:
    /// Keep the mask handles over the picture when the monitor resizes.
    ///
    /// An event filter rather than a layout: the overlay has to cover the
    /// monitor exactly, and a layout that also managed the monitor's own
    /// children would be a second thing deciding where the picture is.
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == monitor_ && event->type() == QEvent::Resize) {
            maskOverlay_->setGeometry(monitor_->rect());
            viewerOverlay_->setGeometry(monitor_->rect());
        }
        return QWidget::eventFilter(watched, event);
    }

    void closeEvent(QCloseEvent* event) override {
        // Handed back on the way out, so the next person is not told a machine
        // that has gone home still has it.
        releaseLock();
        // Never a dialog on the way out.
        //
        // "You have unsaved changes -- save?" is a question whose answer is
        // almost always yes, asked at the moment somebody has already decided
        // to leave. Writing the recovery file instead means quitting is always
        // instant and never loses anything: the next open finds the autosave
        // and offers it back. It also keeps the promise that the file somebody
        // last chose to save stays as they left it.
        autosave();
        saveWorkspace();
        shutDown();
        QWidget::closeEvent(event);
    }

private:
    // --- The chrome ---------------------------------------------------------
    //
    // The window's own furniture: a menu bar, a tool bar, the viewer's header,
    // the transport, the timeline's header and a status line. Built here rather
    // than in a designer file, in the order they are stacked on screen.

    /// A small flat button, which is what most of the chrome is made of.
    QPushButton* chromeButton(const QString& text, const QString& tip, bool checkable = false) {
        auto* button = new QPushButton(text, this);
        button->setToolTip(tip);
        button->setProperty("flat", true);
        button->setCheckable(checkable);
        button->setFocusPolicy(Qt::NoFocus);
        button->setMinimumWidth(0);
        return button;
    }

    /// An icon button, which is what most of the timeline's own controls are.
    ///
    /// Same shape as the tool palette's buttons, so a row that mixes tools with
    /// actions reads as one row rather than two.
    QPushButton* chromeIconButton(app::icons::Glyph glyph, const QString& tip,
                                  bool checkable = false) {
        QPushButton* button = chromeButton({}, tip, checkable);
        button->setIcon(app::icons::toolIcon(glyph));
        button->setIconSize(QSize(17, 17));
        button->setFixedSize(29, 26);
        return button;
    }

    QFrame* chromeSeparator() {
        auto* line = new QFrame(this);
        line->setFrameShape(QFrame::VLine);
        line->setFixedWidth(1);
        line->setStyleSheet(
            QString("background:%1;border:none").arg(app::theme::divider().name(QColor::HexRgb)));
        line->setFixedHeight(20);
        return line;
    }

    QLabel* mutedLabel(const QString& text = {}) {
        auto* label = new QLabel(text, this);
        label->setProperty("muted", true);
        return label;
    }

    template <typename F>
    QAction* menuItem(QMenu* menu, const char* actionId, F&& handler) {
        const ui::ActionInfo* info = ui::findAction(actionId);
        ZARO_CHECK(info != nullptr, "a menu item names an action nobody catalogued");

        QAction* action = menu->addAction(
            QString::fromUtf8(info->label.data(), static_cast<int>(info->label.size())));
        // The id is the object name, so the self-tests and anything else that
        // reaches for a command by name uses the same name the keymap does.
        action->setObjectName(QString::fromUtf8(actionId));
        connect(action, &QAction::triggered, this, std::forward<F>(handler));
        actions_.insert(QString::fromUtf8(actionId), action);
        applyShortcut(actionId, action);
        return action;
    }

    /// The commands that arrive by key rather than through a menu.
    void bindPlaybackActions() {
        bindAction("play-pause", [this] { togglePlay(); });
        bindAction("shuttle-back", [this] {
            transport_.pressJ();
            startIfPlaying();
        });
        bindAction("shuttle-stop", [this] {
            transport_.pressK();
            stop();
        });
        bindAction("shuttle-forward", [this] {
            transport_.pressL();
            startIfPlaying();
        });
        bindAction("step-back", [this] { step(-1); });
        bindAction("step-forward", [this] { step(1); });
        bindAction("go-to-start", [this] {
            stop();
            setPosition(time::RationalTime{0, liveSequence()->frameRate()});
        });
        bindAction("go-to-end", [this] {
            stop();
            setPosition(liveSequence()->duration());
        });
        bindAction("mark-in", [this] { doMarkIn(); });
        bindAction("mark-out", [this] { doMarkOut(); });
        bindAction("insert-from-source", [this] { doInsert(); });
        bindAction("overwrite-from-source", [this] { doOverwrite(); });
        bindAction("source-back", [this] { doSourceBack(); });
        bindAction("source-forward", [this] { doSourceForward(); });
    }

    void buildMenus() {
        menuBar_ = new QMenuBar(this);
        // Native where the platform has a menu bar of its own. The design draws
        // the menus inside the window, which is right on Windows and Linux and
        // wrong on macOS: there the menu bar belongs at the top of the screen,
        // and a second one in the window is a second place to look. Qt decides
        // by platform on its own; this only says so out loud.
#ifdef Q_OS_MACOS
        menuBar_->setNativeMenuBar(true);
#else
        menuBar_->setNativeMenuBar(false);
#endif

        QMenu* file = menuBar_->addMenu("File");
        menuItem(file, "new-project", [this] { newProject(); });
        menuItem(file, "open-project", [this] { openDialog(); });
        file->addSeparator();
        menuItem(file, "save-project", [this] { static_cast<void>(save()); });
        menuItem(file, "save-project-as", [this] { static_cast<void>(saveAs()); });

        // Grouped into submenus rather than listed. Fifteen items and four
        // rules made a File menu taller than some of the panels, and length is
        // what makes a menu hard to read: four of these are about media, two
        // about versions, two about templates and two about getting a file
        // out, and saying so is shorter than spelling every one of them out.
        QMenu* versions = file->addMenu("Versions");
        menuItem(versions, "save-version", [this] {
            auto saved = saveNewVersion();
            if (!saved) {
                app::say(this, "Version", QString::fromStdString(saved.error().message()));
                return;
            }
            // Said out loud: the window title changes too, but a version
            // that appeared to do nothing is one people press twice.
            app::say(this, "Version",
                     QString("Now working in %1")
                         .arg(QString::fromStdString(
                             std::filesystem::path{*saved}.filename().string())));
        });
        menuItem(versions, "open-version", [this] { openVersionMenu(); });
        file->addSeparator();

        QMenu* media = file->addMenu("Media");
        menuItem(media, "import-media", [this] { bin_->importFiles(); });
        menuItem(media, "browse-media", [this] { browseMedia(); });
        media->addSeparator();
        menuItem(media, "relink-media", [this] { relinkDialog(); });
        menuItem(media, "consolidate-media", [this] { consolidateDialog(); });

        QMenu* exports = file->addMenu("Export");
        menuItem(exports, "export-sequence", [this] { exportDialog(); });
        menuItem(exports, "export-otio", [this] { exportOtio(); });

        QMenu* templates = file->addMenu("Templates");
        menuItem(templates, "save-template", [this] { saveTemplateDialog(); });
        menuItem(templates, "place-template", [this] { placeTemplateDialog(); });
        file->addSeparator();
        menuItem(file, "close-window", [this] { close(); });

        QMenu* edit = menuBar_->addMenu("Edit");
        menuItem(edit, "undo", [this] { timeline_->undo(); });
        menuItem(edit, "redo", [this] { timeline_->redo(); });
        edit->addSeparator();
        menuItem(edit, "select-all", [this] { timeline_->selectAll(); });
        edit->addSeparator();
        menuItem(edit, "detect-scenes", [this] { static_cast<void>(detectScenes()); });

        QMenu* clip = menuBar_->addMenu("Clip");
        menuItem(clip, "match-frame", [this] { matchFrame(); });
        menuItem(clip, "make-subclip", [this] { makeSubclip(); });
        clip->addSeparator();
        menuItem(clip, "proxies", [this] { proxyMenu(); });
        menuItem(clip, "multicam", [this] { multicamMenu(); });
        menuItem(clip, "captions", [this] { captionsMenu(); });

        QMenu* sequence = menuBar_->addMenu("Sequence");
        menuItem(sequence, "razor", [this] { timeline_->razorAtPlayhead(); });
        menuItem(sequence, "add-dissolve", [this] { timeline_->addDissolveAtPlayhead(); });
        sequence->addSeparator();
        menuItem(sequence, "render-range", [this] { renderMenu(); });
        menuItem(sequence, "delivery", [this] { deliveryMenu(); });
        menuItem(sequence, "loudness", [this] { loudnessMenu(); });

        QMenu* text = menuBar_->addMenu("Text");
        menuItem(text, "show-transcript", [this] { showTranscript(); });

        QMenu* audio = menuBar_->addMenu("Audio");
        menuItem(audio, "fit-music", [this] {
            const model::Sequence* sequence = liveSequence();
            if (sequence == nullptr) {
                return;
            }
            // The picture's length, less wherever the music starts: what
            // has to be filled is what is left after it comes in.
            const model::Track* track = sequence->findTrack(selectedTrack_);
            const model::Clip* clip = track != nullptr ? track->find(selectedClip_) : nullptr;
            const double from = clip != nullptr ? clip->start().toSecondsDouble() : 0.0;
            const double wanted = sequence->duration().toSecondsDouble() - from;
            auto plan = remixSelectedTo(wanted);
            if (!plan) {
                app::say(this, "Fit music", QString::fromStdString(plan.error().message()));
                return;
            }
            app::say(this, "Fit music",
                     QString("Cut %1 beats out at %2s, now %3s long.")
                         .arg(plan->beatsRemoved)
                         .arg(plan->cutAt, 0, 'f', 2)
                         .arg(plan->seconds, 0, 'f', 2));
        });

        QMenu* marker = menuBar_->addMenu("Marker");
        menuItem(marker, "add-marker", [this] { timeline_->addMarkerAtPlayhead(); });
        menuItem(marker, "next-marker", [this] { doNextMarker(); });
        menuItem(marker, "previous-marker", [this] { doPreviousMarker(); });
        marker->addSeparator();
        menuItem(marker, "resolve-comment", [this] { static_cast<void>(toggleCommentHere()); });
        menuItem(marker, "export-review", [this] { exportReviewNotes(); });

        QMenu* effects = menuBar_->addMenu("Effects");
        compareAction_ = menuItem(effects, "compare", [this](bool on) {
            // Turning it on takes the frame showing now as the reference. That
            // is the gesture: somebody looks at a shot they like and says
            // "against this" -- asking them to nominate one first would be a
            // step between the thought and the thing.
            setComparing(on, on ? position_ : referenceAt_);
        });
        compareAction_->setCheckable(true);
        menuItem(effects, "match-shot", [this] { matchShot(); });

        QMenu* view = menuBar_->addMenu("View");
        QMenu* workspaces = view->addMenu("Workspace");
        for (const QString& name : kWorkspaces) {
            QAction* action = workspaces->addAction(name);
            action->setCheckable(true);
            workspaceActions_.insert(name, action);
            connect(action, &QAction::triggered, this, [this, name] { setWorkspace(name); });
        }
        view->addSeparator();
        menuItem(view, "zoom-in", [this] { timeline_->zoomBy(1.4); });
        menuItem(view, "zoom-out", [this] { timeline_->zoomBy(1.0 / 1.4); });
        menuItem(view, "zoom-fit", [this] { timeline_->zoomToFit(); });
        view->addSeparator();
        guidesAction_ = menuItem(view, "safe-guides", [this](bool on) {
            viewerOverlay_->setGuides(on);
            if (guidesButton_ != nullptr) {
                guidesButton_->setChecked(on);
            }
        });
        guidesAction_->setCheckable(true);

        QMenu* window = menuBar_->addMenu("Window");
        panelAction(window, "Project Bin", [this] { return bin_; });
        panelAction(window, "Effect Controls", [this] { return static_cast<QWidget*>(effects_); });
        panelAction(window, "Scopes", [this] { return static_cast<QWidget*>(scopes_); });
        panelAction(window, "Audio Mixer", [this] { return static_cast<QWidget*>(mixer_); });
        window->addSeparator();
        menuItem(window, "reset-panels", [this] { setWorkspace(workspace_); });

        QMenu* help = menuBar_->addMenu("Help");
        menuItem(help, "hotkeys", [this] { showHotkeys(); });
        menuItem(help, "about", [this] {
            QMessageBox::about(this, "Zaro Video",
                               "Zaro Video — a non-linear editor.\n\n"
                               "C++20, Qt 6, FFmpeg, GPU compositing on Qt RHI.");
        });
    }

    /// A Window-menu item that shows and hides one panel.
    template <typename F>
    void panelAction(QMenu* menu, const QString& text, F&& panel) {
        QAction* action = menu->addAction(text);
        action->setCheckable(true);
        connect(action, &QAction::triggered, this, [panel](bool on) { panel()->setVisible(on); });
        connect(menu, &QMenu::aboutToShow, this,
                [action, panel] { action->setChecked(panel()->isVisible()); });
    }

    QWidget* buildTitleBar() {
        auto* bar = new QWidget(this);
        bar->setObjectName("chrome-titlebar");
        bar->setFixedHeight(38);
        auto* row = new QHBoxLayout(bar);
        row->setContentsMargins(12, 0, 12, 0);
        row->setSpacing(8);

        auto* brand = new QLabel("Zaro", bar);
        brand->setObjectName("chrome-brand");
        row->addWidget(brand);
        if (!menuBar_->isNativeMenuBar()) {
            menuBar_->setParent(bar);
            row->addWidget(menuBar_);
        }
        row->addStretch(1);

        projectLabel_ = mutedLabel();
        row->addWidget(projectLabel_);
        row->addStretch(1);

        autosaveLabel_ = mutedLabel();
        row->addWidget(autosaveLabel_);
        return bar;
    }

    /// The tool palette.
    ///
    /// On the timeline's own header rather than the window's tool bar: every
    /// one of these tools acts on the timeline and nowhere else, and a control
    /// two panels away from the thing it changes is one people stop reaching
    /// for.
    QWidget* buildToolPalette() {
        // The tools, in the order a cut is made: pick, cut, trim, slip, then
        // the two that move the view rather than the cut.
        struct ToolEntry {
            app::TimelineWidget::Tool tool;
            app::icons::Glyph glyph;
            const char* name;
            const char* key;
        };
        static const ToolEntry kTools[] = {
            {app::TimelineWidget::Tool::Select, app::icons::Glyph::Cursor, "Select", "V"},
            {app::TimelineWidget::Tool::Blade, app::icons::Glyph::Scissors, "Blade", "B"},
            {app::TimelineWidget::Tool::Trim, app::icons::Glyph::TrimEdges, "Trim", "T"},
            {app::TimelineWidget::Tool::Slip, app::icons::Glyph::SlipArrows, "Slip", "Y"},
            {app::TimelineWidget::Tool::Hand, app::icons::Glyph::Hand, "Hand", "H"},
            {app::TimelineWidget::Tool::Zoom, app::icons::Glyph::Magnifier, "Zoom", "Z"},
        };
        // No box around it. In the tool bar it needed one to say where the
        // group ended; on the timeline header it is among the other buttons
        // that act on the timeline, and a border there would be drawing a line
        // between things that belong together.
        auto* toolGroup = new QWidget(this);
        auto* toolRow = new QHBoxLayout(toolGroup);
        toolRow->setContentsMargins(2, 2, 2, 2);
        toolRow->setSpacing(2);
        for (const ToolEntry& entry : kTools) {
            // The tooltip carries the key. A tool palette is aimed at rather
            // than read -- the shape is what somebody learns -- and the letter
            // is one hover away for as long as it takes to learn it.
            QPushButton* button =
                chromeButton({}, QString("%1 tool (%2)").arg(entry.name, entry.key), true);
            button->setIcon(app::icons::toolIcon(entry.glyph));
            button->setIconSize(QSize(17, 17));
            button->setFixedSize(29, 26);
            const auto tool = entry.tool;
            connect(button, &QPushButton::clicked, this,
                    [this, tool] { timeline_->setTool(tool); });
            toolButtons_.push_back(button);
            toolRow->addWidget(button);
        }
        return toolGroup;
    }

    QWidget* buildToolBar() {
        auto* bar = new QWidget(this);
        bar->setObjectName("chrome-toolbar");
        bar->setFixedHeight(46);
        auto* row = new QHBoxLayout(bar);
        row->setContentsMargins(12, 0, 12, 0);
        row->setSpacing(10);

        // Snapping and markers used to be here. They belong with the timeline
        // -- both of them are about where an edit lands, and the timeline is
        // where edits land -- so they moved down with the tools.
        formatLabel_ = mutedLabel();
        row->addWidget(formatLabel_);
        row->addStretch(1);

        auto* tabGroup = new QWidget(bar);
        tabGroup->setObjectName("tab-group");
        // Fixed, and centred: without a height the group stretches to the whole
        // bar, so its pill ran from the top edge to the bottom while the
        // buttons beside it were thirty pixels tall in the middle.
        tabGroup->setFixedHeight(30);
        auto* tabRow = new QHBoxLayout(tabGroup);
        tabRow->setContentsMargins(2, 2, 2, 2);
        tabRow->setSpacing(2);
        for (const QString& name : kWorkspaces) {
            QPushButton* tab = chromeButton(name, QString("%1 workspace").arg(name), true);
            tab->setFixedHeight(26);
            connect(tab, &QPushButton::clicked, this, [this, name] { setWorkspace(name); });
            workspaceTabs_.insert(name, tab);
            tabRow->addWidget(tab);
        }
        row->addWidget(tabGroup, 0, Qt::AlignVCenter);
        row->addStretch(1);

        // Two clusters, one shown at a time: Import and Export belong to the
        // workspaces where there is something to import into, and the queue
        // buttons belong to Deliver. A bar that showed all four would offer
        // Export and Start render side by side, which are the same intention
        // asked twice.
        actionStack_ = new QStackedWidget(bar);
        actionStack_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

        auto* editActions = new QWidget(actionStack_);
        auto* editRow = new QHBoxLayout(editActions);
        editRow->setContentsMargins(0, 0, 0, 0);
        editRow->setSpacing(8);
        auto* importButton = new QPushButton("Import", editActions);
        importButton->setFixedHeight(30);
        connect(importButton, &QPushButton::clicked, this, [this] { bin_->importFiles(); });
        editRow->addWidget(importButton);
        auto* exportButton = new QPushButton("Export", editActions);
        exportButton->setProperty("accent", true);
        exportButton->setFixedHeight(30);
        connect(exportButton, &QPushButton::clicked, this, [this] { exportDialog(); });
        editRow->addWidget(exportButton);
        actionStack_->addWidget(editActions);

        auto* deliverActions = new QWidget(actionStack_);
        auto* deliverRow = new QHBoxLayout(deliverActions);
        deliverRow->setContentsMargins(0, 0, 0, 0);
        deliverRow->setSpacing(8);
        auto* addToQueue = new QPushButton("Add to queue", deliverActions);
        addToQueue->setFixedHeight(30);
        connect(addToQueue, &QPushButton::clicked, this, [this] { deliver_->queueCurrent(); });
        deliverRow->addWidget(addToQueue);
        renderButton_ = new QPushButton("Start render", deliverActions);
        renderButton_->setProperty("accent", true);
        renderButton_->setFixedHeight(30);
        connect(renderButton_, &QPushButton::clicked, this, [this] {
            deliver_->toggleRendering();
            updateChrome();
        });
        deliverRow->addWidget(renderButton_);
        actionStack_->addWidget(deliverActions);
        row->addWidget(actionStack_);

        auto* donate = new app::SupportButton(bar);
        donate->setObjectName("donate");
        donate->setText("Donate");
        donate->setToolTip(QString("Support Zaro — opens %1").arg(kSupportUrl));
        donate->setFixedHeight(30);
        connect(donate, &QPushButton::clicked, this,
                [] { static_cast<void>(QDesktopServices::openUrl(QUrl{kSupportUrl})); });
        row->addWidget(donate);
        return bar;
    }

    QWidget* buildViewerBar() {
        auto* bar = new QWidget(this);
        bar->setObjectName("chrome-viewer-bar");
        bar->setFixedHeight(34);
        auto* row = new QHBoxLayout(bar);
        row->setContentsMargins(12, 0, 12, 0);
        row->setSpacing(8);

        auto* segment = new QWidget(bar);
        segment->setObjectName("segment-group");
        segment->setFixedHeight(28);
        auto* segmentRow = new QHBoxLayout(segment);
        segmentRow->setContentsMargins(2, 2, 2, 2);
        segmentRow->setSpacing(2);
        sourceTab_ = chromeButton("Source", "The clip opened from the bin", true);
        programTab_ = chromeButton("Program", "The sequence at the playhead", true);
        for (QPushButton* tab : {sourceTab_, programTab_}) {
            tab->setFixedHeight(24);
            segmentRow->addWidget(tab);
        }
        // The dot is what says "toggle" rather than "tab": two tabs in a group
        // mean one of them is on, and these two are independent.
        for (QPushButton* tab : {sourceTab_, programTab_}) {
            tab->setIconSize(QSize(13, 13));
        }
        connect(sourceTab_, &QPushButton::toggled, this, [this](bool on) { setSourceShown(on); });
        connect(programTab_, &QPushButton::toggled, this, [this](bool on) { setProgramShown(on); });
        syncViewers();
        row->addWidget(segment, 0, Qt::AlignVCenter);

        viewerLabel_ = mutedLabel();
        row->addWidget(viewerLabel_);
        row->addStretch(1);

        guidesButton_ = chromeButton("Guides", "Action-safe, title-safe and the thirds", true);
        guidesButton_->setFixedHeight(24);
        connect(guidesButton_, &QPushButton::clicked, this, [this](bool on) {
            viewerOverlay_->setGuides(on);
            guidesAction_->setChecked(on);
        });
        row->addWidget(guidesButton_);

        qualityLabel_ = mutedLabel();
        row->addWidget(qualityLabel_);
        return bar;
    }

    QWidget* buildTransportBar() {
        auto* bar = new QWidget(this);
        bar->setObjectName("chrome-transport");
        auto* column = new QVBoxLayout(bar);
        column->setContentsMargins(14, 6, 14, 8);
        column->setSpacing(6);
        column->addWidget(scrubber_);

        auto* row = new QHBoxLayout;
        row->setSpacing(4);
        timecode_->setMinimumWidth(140);
        row->addWidget(timecode_);
        row->addStretch(1);

        struct TransportEntry {
            const char* glyph;
            const char* tip;
            void (PreviewWindow::*action)();
        };
        static const TransportEntry kBefore[] = {
            {"|◀", "Go to the start (Home)", &PreviewWindow::goToStart},
            {"◁", "Back one frame (Left)", &PreviewWindow::stepBack},
        };
        static const TransportEntry kAfter[] = {
            {"▷", "Forward one frame (Right)", &PreviewWindow::stepForward},
            {"▶|", "Go to the end (End)", &PreviewWindow::goToEnd},
        };
        const auto addTransport = [&](const TransportEntry& entry) {
            QPushButton* button = chromeButton(entry.glyph, entry.tip);
            button->setFixedSize(32, 30);
            const auto action = entry.action;
            connect(button, &QPushButton::clicked, this, [this, action] { (this->*action)(); });
            row->addWidget(button);
        };
        for (const TransportEntry& entry : kBefore) {
            addTransport(entry);
        }
        row->addWidget(playButton_);
        for (const TransportEntry& entry : kAfter) {
            addTransport(entry);
        }

        row->addWidget(chromeSeparator());
        QPushButton* markIn = chromeButton("[", "Mark in (I)");
        markIn->setFixedSize(30, 30);
        connect(markIn, &QPushButton::clicked, this, [this] { doMarkIn(); });
        row->addWidget(markIn);
        QPushButton* markOut = chromeButton("]", "Mark out (O)");
        markOut->setFixedSize(30, 30);
        connect(markOut, &QPushButton::clicked, this, [this] { doMarkOut(); });
        row->addWidget(markOut);

        row->addStretch(1);
        remaining_->setMinimumWidth(140);
        remaining_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(remaining_);
        column->addLayout(row);
        return bar;
    }

    QWidget* buildTimelinePane() {
        auto* pane = new QWidget(this);
        auto* column = new QVBoxLayout(pane);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(0);

        auto* bar = new QWidget(pane);
        bar->setObjectName("chrome-timeline-bar");
        bar->setFixedHeight(34);
        auto* row = new QHBoxLayout(bar);
        row->setContentsMargins(10, 0, 10, 0);
        row->setSpacing(8);
        auto* title = new QLabel("Timeline", bar);
        row->addWidget(title);
        timelineLabel_ = mutedLabel();
        row->addWidget(timelineLabel_);
        row->addWidget(chromeSeparator());
        row->addWidget(buildToolPalette());

        QPushButton* razor =
            chromeIconButton(app::icons::Glyph::Split, "Razor at the playhead (C)");
        connect(razor, &QPushButton::clicked, this, [this] { timeline_->razorAtPlayhead(); });
        row->addWidget(razor);

        QPushButton* dissolve = chromeIconButton(app::icons::Glyph::CrossFade,
                                                 "Put a dissolve on the cut at the playhead");
        connect(dissolve, &QPushButton::clicked, this,
                [this] { timeline_->addDissolveAtPlayhead(); });
        row->addWidget(dissolve);

        row->addWidget(chromeSeparator());

        snapButton_ = chromeIconButton(app::icons::Glyph::Magnet,
                                       "Pull edits to the edit points near them (S)", true);
        connect(snapButton_, &QPushButton::clicked, this,
                [this](bool on) { timeline_->setSnapEnabled(on); });
        row->addWidget(snapButton_);

        QPushButton* markerButton =
            chromeIconButton(app::icons::Glyph::Bookmark, "Add a marker at the playhead (M)");
        connect(markerButton, &QPushButton::clicked, this,
                [this] { timeline_->addMarkerAtPlayhead(); });
        row->addWidget(markerButton);

        row->addStretch(1);
        snapLabel_ = mutedLabel();
        row->addWidget(snapLabel_);
        auto* zoomOut = chromeIconButton(app::icons::Glyph::Minus, "Zoom out (−)");
        connect(zoomOut, &QPushButton::clicked, this, [this] { timeline_->zoomBy(1.0 / 1.4); });
        row->addWidget(zoomOut);
        zoomSlider_ = new QSlider(Qt::Horizontal, bar);
        zoomSlider_->setFixedWidth(96);
        zoomSlider_->setRange(0, 1000);
        zoomSlider_->setFocusPolicy(Qt::NoFocus);
        connect(zoomSlider_, &QSlider::valueChanged, this, [this](int value) {
            if (zoomSlider_->isSliderDown()) {
                timeline_->setZoomFraction(value / 1000.0);
            }
        });
        row->addWidget(zoomSlider_);
        auto* zoomIn = chromeIconButton(app::icons::Glyph::Plus, "Zoom in (+)");
        connect(zoomIn, &QPushButton::clicked, this, [this] { timeline_->zoomBy(1.4); });
        row->addWidget(zoomIn);

        column->addWidget(bar);
        column->addWidget(timeline_, 1);
        return pane;
    }

    QWidget* buildStatusBar() {
        auto* bar = new QWidget(this);
        bar->setObjectName("chrome-statusbar");
        bar->setFixedHeight(26);
        auto* row = new QHBoxLayout(bar);
        row->setContentsMargins(12, 0, 12, 0);
        row->setSpacing(16);
        statusLeft_ = mutedLabel();
        statusMiddle_ = mutedLabel();
        statusRight_ = mutedLabel();
        row->addWidget(statusLeft_);
        row->addWidget(statusMiddle_);
        row->addStretch(1);
        row->addWidget(statusRight_);
        return bar;
    }

    /// Show what is turned on, and say so on the toggles and in the well.
    ///
    /// One place, so the toggles and the monitors cannot disagree: every way in
    /// -- a click on a chip, a clip opened from the bin, a match frame -- ends
    /// up here rather than setting visibility itself.
    void syncViewers() {
        source_->setVisible(sourceShown_);
        monitor_->setVisible(programShown_);
        noMonitorLabel_->setVisible(!sourceShown_ && !programShown_);
        // These do not re-enter: setChecked only emits when the value moves,
        // and by here it already is what it is being set to.
        sourceTab_->setChecked(sourceShown_);
        programTab_->setChecked(programShown_);
        sourceTab_->setIcon(app::icons::toolIcon(
            sourceShown_ ? app::icons::Glyph::CheckCircle : app::icons::Glyph::Circle, 13));
        programTab_->setIcon(app::icons::toolIcon(
            programShown_ ? app::icons::Glyph::CheckCircle : app::icons::Glyph::Circle, 13));
    }

public:
    /// Turn the source monitor on. Opening a clip from the bin and match frame
    /// both want it up; neither wants the program taken down to get it, which
    /// is what a stack used to make them do.
    void setSourceShown(bool on) {
        sourceShown_ = on;
        syncViewers();
    }
    void setProgramShown(bool on) {
        programShown_ = on;
        syncViewers();
    }

    /// Put the program monitor up. Kept under its old name because it is the
    /// same errand -- make sure the cut is on screen -- and it no longer costs
    /// the source to do it.
    void showProgram() { setProgramShown(true); }

    /// Where a workspace's splitter sizes are remembered.
    ///
    /// Versioned, because restoring a size list into a splitter with a
    /// different number of panes leaves the new ones at zero width -- which
    /// looks exactly like a panel that failed to appear. Bump the number when
    /// panes are added or removed from either splitter.
    static QString layoutKey(const QString& workspace, const char* which) {
        return QString("workspace/%1/%2-v3").arg(workspace, QString::fromUtf8(which));
    }

    /// A workspace is which panels are up. Four arrangements, because there are
    /// four things people do with an editor, and each of them wants a different
    /// half of the window: the panels a colourist needs are dead weight while
    /// somebody is assembling, and the reverse.
    void setWorkspace(const QString& name) {
        if (!kWorkspaces.contains(name)) {
            return;
        }
        // The arrangement of the workspace being left is remembered, so coming
        // back to it finds the splitters where they were.
        if (topSplitter_ != nullptr && !workspace_.isEmpty()) {
            QSettings settings("Zaro", "Zaro Video");
            settings.setValue(layoutKey(workspace_, "top"), topSplitter_->saveState());
            settings.setValue(layoutKey(workspace_, "main"), mainSplitter_->saveState());
        }
        workspace_ = name;

        const bool colour = name == "Color";
        const bool audio = name == "Audio";
        const bool deliver = name == "Deliver";
        if (workspaceStack_ != nullptr) {
            workspaceStack_->setCurrentIndex(deliver ? 1 : 0);
            actionStack_->setCurrentIndex(deliver ? 1 : 0);
            if (deliver) {
                deliver_->setProject(&project_, sequenceId_);
                deliver_->setPlayhead(position_);
            }
        }
        // The bin and the parameter panel are both about picture; Audio has a
        // console in the middle and a channel's chain on the right, and neither
        // of those wants a clip's motion controls beside it.
        bin_->setVisible(!colour && !deliver && !audio);
        effects_->setVisible(!deliver && !audio);
        scopes_->setVisible(colour);
        mixer_->setVisible(audio);
        // Color is a different room: the gallery and the shot strip replace the
        // bin and the timeline, the grade chain sits over the parameters, and
        // the wheels take the bottom of the window.
        gallery_->setVisible(colour);
        clipStrip_->setVisible(colour);
        nodesBox_->setVisible(colour);
        palette_->setVisible(colour);
        timelinePane_->setVisible(!colour && !deliver);
        // Audio is a console: the mixer takes the centre, the loudness meter
        // and the channel's chain take the sides, and the picture stands down.
        audioSide_->setVisible(audio);
        channel_->setVisible(audio);
        viewerWell_->setVisible(!audio && !deliver);
        viewerBar_->setVisible(!audio && !deliver);
        if (audio) {
            mixer_->setProject(&project_, sequenceId_, &commands_);
            channel_->setProject(&project_, sequenceId_, &commands_);
            channel_->setTrack(mixer_->picked());
            stems_->setProject(&project_, sequenceId_);
            refreshInstruments();
        }
        if (colour) {
            palette_->setProject(&project_, sequenceId_, &commands_);
            palette_->setSelection(selectedTrack_, selectedClip_);
            clipStrip_->setProject(&project_, sequenceId_);
            clipStrip_->setSelection(selectedTrack_, selectedClip_);
            refreshGradeChain();
        }

        for (auto entry = workspaceTabs_.constBegin(); entry != workspaceTabs_.constEnd();
             ++entry) {
            entry.value()->setChecked(entry.key() == name);
        }
        for (auto entry = workspaceActions_.constBegin(); entry != workspaceActions_.constEnd();
             ++entry) {
            entry.value()->setChecked(entry.key() == name);
        }

        QSettings settings("Zaro", "Zaro Video");
        if (const auto state = settings.value(layoutKey(name, "top")).toByteArray();
            !state.isEmpty()) {
            topSplitter_->restoreState(state);
        }
        if (const auto state = settings.value(layoutKey(name, "main")).toByteArray();
            !state.isEmpty()) {
            mainSplitter_->restoreState(state);
        }
        updateChrome();
    }

private:
    /// Everything in the chrome that describes state rather than causing it.
    void updateChrome() {
        const model::Sequence* sequence = liveSequence();
        const QString name = path_.empty()
                                 ? QString{"Untitled"}
                                 : QFileInfo(QString::fromStdString(path_)).completeBaseName();
        projectLabel_->setText(
            QString("%1 · %2%3")
                .arg(name,
                     sequence != nullptr ? QString::fromStdString(sequence->name()) : QString{"—"},
                     commands_.isModified() ? " •" : ""));
        autosaveLabel_->setText(commands_.isModified() ? "Unsaved changes" : "Saved");

        if (sequence != nullptr) {
            const double fps = sequence->frameRate().toDouble();
            formatLabel_->setText(QString("%1×%2 · %3 fps · Rec.709")
                                      .arg(sequence->width())
                                      .arg(sequence->height())
                                      .arg(fps, 0, 'g', 5));
            const bool dropFrame = time::supportsDropFrame(sequence->frameRate());
            const time::Timecode duration = time::timecodeFromFrames(
                sequence->duration().frames(), sequence->frameRate(), dropFrame);
            timelineLabel_->setText(
                QString("%1 · %2").arg(QString::fromStdString(sequence->name()),
                                       QString::fromStdString(duration.toString())));
            viewerLabel_->setText(
                QString("%1 — %2").arg(name, QString::fromStdString(sequence->name())));
        }
        qualityLabel_->setText(monitor_->comparing() ? "Compare · CPU" : "Full · GPU");

        static const QString kToolNames[] = {"Select", "Blade", "Trim", "Slip", "Hand", "Zoom"};
        statusLeft_->setText(QString("%1 tool · %2 workspace")
                                 .arg(kToolNames[static_cast<int>(timeline_->tool())], workspace_));
        const int items = bin_->count();
        if (workspace_ == "Deliver" && deliver_ != nullptr) {
            // In Deliver the interesting middle fact is the queue, not the bin,
            // and the tool bar's left label is the range rather than the format.
            statusMiddle_->setText(deliver_->statusSummary());
            formatLabel_->setText(deliver_->rangeSummary());
            renderButton_->setText(deliver_->rendering() ? "Stop render" : "Start render");
        } else {
            statusMiddle_->setText(QString("%1 %2 · %3")
                                       .arg(items)
                                       .arg(items == 1 ? "item" : "items",
                                            commands_.isModified() ? "edited" : "clean"));
        }
        statusRight_->setText(QString("%1 · Qt %2").arg(kPlatformLabel, QT_VERSION_STR));

        snapButton_->setChecked(timeline_->snapEnabled());
        snapLabel_->setText(timeline_->snapEnabled() ? "Snap on" : "Snap off");
        const auto tool = static_cast<std::size_t>(timeline_->tool());
        for (std::size_t i = 0; i < toolButtons_.size(); ++i) {
            toolButtons_[i]->setChecked(i == tool);
        }
        if (!zoomSlider_->isSliderDown()) {
            const QSignalBlocker blocker{zoomSlider_};
            zoomSlider_->setValue(static_cast<int>(timeline_->zoomFraction() * 1000.0));
        }
    }

    /// Two menu items and a toolbar button that were buttons in a row before.
    void exportDialog() {
        if (liveSequence() == nullptr) {
            return;
        }
        stop();
        app::ExportDialog dialog{project_, liveSequence()->id(), this};
        dialog.exec();
    }

    void exportOtio() {
        if (liveSequence() == nullptr) {
            return;
        }
        // Export only, from the window. Importing an OTIO file produces a
        // project of its own, and replacing the open one needs a "save first?"
        // that does not exist yet -- so that direction lives in zaro-otio,
        // where there is nothing to lose.
        const QString path = QFileDialog::getSaveFileName(
            this, "Export OpenTimelineIO", "timeline.otio", "OpenTimelineIO (*.otio)");
        if (path.isEmpty()) {
            return;
        }
        if (Status saved = io::saveOtio(project_, liveSequence()->id(), path.toStdString());
            !saved) {
            app::warn(this, "OpenTimelineIO", QString::fromStdString(saved.error().toString()));
        }
    }

    void trackMask() {
        // Every frame of the rest of the clip gets composited, which is not
        // instant on a long one.
        QApplication::setOverrideCursor(Qt::WaitCursor);
        auto tracked = trackMaskForward();
        QApplication::restoreOverrideCursor();
        if (!tracked) {
            app::say(this, "Track mask", QString::fromStdString(tracked.error().message()));
            return;
        }
        QString said = QString("Tracked %1 frame%2, weakest match %3.")
                           .arg(tracked->frames)
                           .arg(tracked->frames == 1 ? "" : "s")
                           .arg(tracked->confidence, 0, 'f', 2);
        if (!tracked->stopped.empty()) {
            // Said plainly, with the keyframes kept: where a track gave up is
            // exactly where somebody needs to look.
            said += QString("\nStopped early: %1").arg(QString::fromStdString(tracked->stopped));
        }
        app::say(this, "Track mask", said);
    }

    void stabilise() {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        auto held = stabiliseClip();
        QApplication::restoreOverrideCursor();
        if (!held) {
            app::say(this, "Stabilise", QString::fromStdString(held.error().message()));
            return;
        }
        QString said = QString("Stabilised %1 frames, zoomed in %2%.")
                           .arg(held->measured)
                           .arg((held->zoom - 1.0) * 100.0, 0, 'f', 1);
        if (!held->stopped.empty()) {
            said += QString("\nStopped early: %1").arg(QString::fromStdString(held->stopped));
        }
        app::say(this, "Stabilise", said);
    }

    void clearStabilisation() {
        const model::Sequence* sequence = liveSequence();
        if (sequence == nullptr || !selectedClip_.isValid()) {
            return;
        }
        auto built = edit::makeStabilise(project_, {sequence->id(), selectedTrack_}, selectedClip_,
                                         model::Curve{}, model::Curve{}, 1.0);
        if (!built) {
            return;
        }
        commands_.execute(project_, std::move(*built));
        commands_.breakMerge();
        renderCache_.clear();
        monitor_->update();
        effects_->refresh();
        updateCacheBar();
        updateTitle();
    }

    void relinkDialog() {
        const QString root = QFileDialog::getExistingDirectory(this, "Look for missing media in");
        if (root.isEmpty()) {
            return;
        }
        QApplication::setOverrideCursor(Qt::WaitCursor);
        auto report = relinkMedia(root.toStdString());
        QApplication::restoreOverrideCursor();
        if (!report) {
            app::warn(this, "Relink", QString::fromStdString(report.error().message()));
            return;
        }
        std::size_t byName = 0;
        for (const zaro::io::RelinkMatch& match : report->matches) {
            if (!match.byContent) {
                ++byName;
            }
        }
        QString said =
            QString("Relinked %1 of %2 missing file%3, from %4 looked at.")
                .arg(report->matches.size())
                .arg(report->matches.size() + report->stillMissing.size())
                .arg(report->matches.size() + report->stillMissing.size() == 1 ? "" : "s")
                .arg(report->examined);
        if (byName > 0) {
            // Said plainly: a file matched only by its name is one somebody
            // should look at before trusting the cut.
            said +=
                QString("\n%1 matched by name only -- check they are the right takes.").arg(byName);
        }
        app::say(this, "Relink", said);
    }

    void consolidateDialog() {
        const QString into =
            QFileDialog::getExistingDirectory(this, "Gather the project's media into");
        if (into.isEmpty()) {
            return;
        }
        QApplication::setOverrideCursor(Qt::WaitCursor);
        auto report = consolidateMedia(into.toStdString());
        QApplication::restoreOverrideCursor();
        if (!report) {
            app::warn(this, "Consolidate", QString::fromStdString(report.error().message()));
            return;
        }
        QString said = QString("Gathered %1 file%2, %3 MB copied.")
                           .arg(report->files.size())
                           .arg(report->files.size() == 1 ? "" : "s")
                           .arg(static_cast<double>(report->bytes) / (1024.0 * 1024.0), 0, 'f', 1);
        if (!report->missing.empty()) {
            // Named, because a consolidate that quietly left files behind is
            // an archive somebody will discover is incomplete much later.
            said += QString("\n%1 could not be found -- relink them first.")
                        .arg(report->missing.size());
        }
        app::say(this, "Consolidate", said);
    }

    void saveTemplateDialog() {
        const QString path = QFileDialog::getSaveFileName(this, "Save graphic as template", {},
                                                          "Graphic template (*.zarograph)");
        if (path.isEmpty()) {
            return;
        }
        if (Status saved = saveGraphicTemplate(path.toStdString()); !saved) {
            app::warn(this, "Template", QString::fromStdString(saved.error().message()));
        }
    }

    void placeTemplateDialog() {
        const QString path = QFileDialog::getOpenFileName(this, "Place graphic template", {},
                                                          "Graphic template (*.zarograph)");
        if (path.isEmpty()) {
            return;
        }
        auto placed = placeGraphicTemplate(path.toStdString(), time::RationalTime{});
        if (!placed) {
            app::warn(this, "Template", QString::fromStdString(placed.error().message()));
        }
    }

    void exportReviewNotes() {
        const QString chosen =
            QFileDialog::getSaveFileName(this, "Export review notes", {}, "Markdown (*.md)");
        if (chosen.isEmpty()) {
            return;
        }
        if (Status written = writeReviewNotes(chosen.toStdString()); !written) {
            app::warn(this, "Review", QString::fromStdString(written.error().message()));
        }
    }

    void matchShot() {
        auto match = matchToReference();
        if (!match) {
            app::say(this, "Match", QString::fromStdString(match.error().message()));
            return;
        }
        if (!match->usable) {
            // Said, not applied. The person looking at both frames decides.
            app::say(this, "Match", QString::fromStdString(match->reason));
            return;
        }
        app::say(this, "Match",
                 QString("Matched: the two shots were %1 apart and are now %2.")
                     .arg(match->before, 0, 'f', 3)
                     .arg(match->after, 0, 'f', 3));
    }

    void goToStart() {
        stop();
        setPosition(time::RationalTime{0, liveSequence()->frameRate()});
    }
    void goToEnd() {
        stop();
        setPosition(liveSequence()->duration());
    }
    void stepBack() { step(-1); }
    void stepForward() { step(1); }

    /// Panel sizes and window geometry, remembered between sessions.
    ///
    /// Saved on close rather than continuously: writing settings on every drag
    /// of a splitter is a lot of disk traffic for something only read once.
    void saveWorkspace() {
        QSettings settings("Zaro", "Zaro Video");
        settings.setValue("window/geometry", saveGeometry());
        // Per workspace, because the panels differ between them: one saved
        // arrangement restored into a different set of visible panels is a
        // collapsed bin and a mixer four pixels tall.
        settings.setValue("workspace/current", workspace_);
        settings.setValue(layoutKey(workspace_, "top"), topSplitter_->saveState());
        settings.setValue(layoutKey(workspace_, "main"), mainSplitter_->saveState());
    }

    void restoreWorkspace() {
        QSettings settings("Zaro", "Zaro Video");
        // Each restored only if it was stored, so a first run gets the
        // stretch factors set above rather than a collapsed layout.
        if (const auto geometry = settings.value("window/geometry").toByteArray();
            !geometry.isEmpty()) {
            restoreGeometry(geometry);
        }
        const QString wanted = settings.value("workspace/current", "Edit").toString();
        // Always through setWorkspace, so the panels, the tabs and the splitter
        // states are one decision rather than three that can disagree.
        workspace_.clear();
        setWorkspace(kWorkspaces.contains(wanted) ? wanted : QString{"Edit"});
    }

    /// Stop everything and join. Called from both the close event and the
    /// destructor, because they are not the same path: quitting with Cmd+Q
    /// destroys the window without ever delivering a close event, and a
    /// std::thread destroyed while still joinable calls std::terminate. That
    /// was a real crash -- the application aborted on quit whenever a waveform
    /// scan was still running, which on a freshly opened project is always.
    void shutDown() {
        shuttingDown_.store(true, std::memory_order_relaxed);
        stop();
        if (waveformThread_.joinable()) {
            waveformThread_.join();
        }
    }

    void doNextMarker() {
        if (const model::Marker* marker = liveSequence()->markerAfter(position_)) {
            stop();
            setPosition(marker->range.start());
        }
    }
    void doPreviousMarker() {
        if (const model::Marker* marker = liveSequence()->markerBefore(position_)) {
            stop();
            setPosition(marker->range.start());
        }
    }

    /// Open the source monitor on the frame the selected clip is showing.
    ///
    /// The mapping is `Clip::activeSourceTimeAt` -- the same one the renderer
    /// asks -- so match frame stays right through trims, speed, reverse, a
    /// multicam angle and a time remap, with nothing to keep in step.
    void doMatchFrame() { matchFrame(); }
    void doDetectScenes() { static_cast<void>(detectScenes()); }
    void doNew() { newProject(); }
    void doOpen() { openDialog(); }
    void doSave() { static_cast<void>(save()); }
    void doSaveAs() { static_cast<void>(saveAs()); }
    void doMarkIn() { source_->markIn(); }
    void doMarkOut() { source_->markOut(); }
    void doInsert() { placeFromSource(edit::PlaceMode::Insert); }
    void doOverwrite() { placeFromSource(edit::PlaceMode::Overwrite); }
    void doSourceBack() { source_->step(-1); }
    void doSourceForward() { source_->step(1); }

    /// Three-point edit: the marked range from the source, at the playhead.
    void placeFromSource(edit::PlaceMode mode) {
        const auto range = source_->markedRange();
        if (!range || !source_->media().isValid()) {
            return;
        }
        const auto& videoTracks = liveSequence()->videoTracks();
        if (videoTracks.empty()) {
            return;
        }
        auto built =
            edit::makePlaceFromSource(project_, {liveSequence()->id(), videoTracks.front().id()},
                                      source_->media(), *range, position_, mode);
        if (!built) {
            return;
        }
        commands_.execute(project_, std::move(*built));
        commands_.breakMerge();
        scrubber_->setRange(0, static_cast<int>(liveSequence()->duration().frames()));
        timeline_->update();
        monitor_->update();
        refresh();
    }

    void togglePlay() {
        if (playing_) {
            transport_.pause();
            stop();
        } else {
            transport_.play();
            startIfPlaying();
        }
    }

    void startIfPlaying() {
        if (transport_.speed().isZero()) {
            stop();
            return;
        }
        if (playing_) {
            // Already running; the new speed is picked up from the transport on
            // the next tick, re-anchored so the playhead does not jump.
            anchorPosition_ = position_;
            anchorClock_ = sink_ ? sink_->clockFrames() : 0;
            return;
        }
        startClock();
    }

    void startClock() {
        const time::Rational& audioRate = liveSequence()->audioSampleRate();
        if (!sink_) {
            auto opened = platform::sdl::AudioSink::open(audioRate, 2);
            if (opened) {
                sink_ = std::move(*opened);
            }
        }
        anchorPosition_ = position_;
        anchorClock_ = sink_ ? sink_->clockFrames() : 0;
        audioWritten_ = sink_ ? sink_->clockFrames() : 0;
        playing_ = true;
        playButton_->setText(kPauseGlyph);

        if (sink_) {
            // Audio on its own thread, for the reason ADR-006 records: sharing
            // one with rendering starves the device the moment a frame is slow.
            audioRunning_.store(true, std::memory_order_relaxed);
            audioThread_ = std::thread{[this] { pumpAudio(); }};
            sink_->start();
        }
        clockTimer_->start();
    }

    void stop() {
        if (!playing_) {
            return;
        }
        playing_ = false;
        clockTimer_->stop();
        audioRunning_.store(false, std::memory_order_relaxed);
        if (audioThread_.joinable()) {
            audioThread_.join();
        }
        if (sink_) {
            sink_->pause();
        }
        playButton_->setText(kPlayGlyph);
    }

    /// Peaks are generated off the UI thread: decoding a long file's audio
    /// takes seconds, and a project that freezes while it opens is worse than
    /// one whose waveforms arrive a moment late.
    void startWaveforms() {
        // A scan may already be running -- reopening the media does this, which
        // is what switching to proxies does. Assigning over a joinable thread
        // is an immediate std::terminate, and that is exactly how it presented:
        // the application aborted the moment proxies were switched on.
        if (waveformThread_.joinable()) {
            waveformThread_.join();
        }

        const std::filesystem::path cacheDirectory =
            std::filesystem::temp_directory_path() / "zaro" / "waveforms";

        // Every media reference is tried, rather than only those the project
        // file says have audio. That cached info is a cache -- it can be stale,
        // absent, or written by a build that did not record it -- and deciding
        // whether to look at a file based on it means a missing field silently
        // becomes a missing waveform. Files without audio simply fail and are
        // skipped.
        std::vector<std::pair<model::MediaRefId, std::string>> wanted;
        for (const model::MediaRef& ref : project_.media()) {
            wanted.emplace_back(ref.id, ref.path);
        }
        if (wanted.empty()) {
            return;
        }

        waveformThread_ = std::thread{[this, wanted, cacheDirectory] {
            platform::ffmpeg::WaveformStore store{cacheDirectory.string()};
            const auto keepGoing = [this] {
                return !shuttingDown_.load(std::memory_order_relaxed);
            };
            for (const auto& [id, path] : wanted) {
                if (!keepGoing()) {
                    return;
                }
                // Cancellable mid-file, not just between files: one long clip
                // is the common case, so checking only at file boundaries would
                // still make quitting wait for the whole scan.
                auto built = store.get(path, 512, keepGoing);
                if (!built) {
                    continue;
                }
                auto shared = std::make_shared<const media::Waveform>(std::move(*built));
                // Back to the UI thread to hand it over: the widget is not
                // thread safe and neither is repainting.
                QMetaObject::invokeMethod(
                    this, [this, id, shared] { timeline_->setWaveform(id, shared); },
                    Qt::QueuedConnection);
            }
        }};
    }

    void pumpAudio() {
        render::AudioGraph mixer{*media_};
        // A compressor's envelope and a filter's delay line are state, so
        // starting playback somewhere new has to begin from silence -- else the
        // first moment after a jump is ducked by whatever was loud wherever the
        // playhead was before.
        mixer.resetProcessing();
        const time::Rational& audioRate = liveSequence()->audioSampleRate();
        constexpr std::int32_t kChannels = 2;

        while (audioRunning_.load(std::memory_order_relaxed)) {
            const std::int64_t target = sink_->clockFrames() + sink_->deviceBufferFrames() * 3;
            while (audioWritten_ < target && audioRunning_.load(std::memory_order_relaxed)) {
                const std::int64_t block = std::min<std::int64_t>(1024, target - audioWritten_);
                if (sink_->ring().availableToWrite() < block) {
                    break;
                }
                std::vector<float> interleaved(static_cast<std::size_t>(block * kChannels), 0.0F);

                // Only at 1x: shuttling needs pitch handling to sound like
                // anything, so it runs silent while the clock keeps time.
                if (transport_.speed() == time::Rational::fromInt(1)) {
                    const auto from = anchorPosition_.rescaledTo(audioRate) +
                                      time::RationalTime{audioWritten_ - anchorClock_, audioRate};
                    if (auto mixed = mixer.mix(*liveSequence(), from, block, kChannels)) {
                        {
                            // Copied under a lock for the UI to read. This
                            // thread already allocates a buffer per block, so
                            // it is not a realtime context -- the realtime path
                            // is the device callback draining the ring.
                            const std::lock_guard<std::mutex> guard{meterMutex_};
                            latestMeters_ = mixer.meters();
                        }
                        for (std::int64_t i = 0; i < mixed->sampleCount(); ++i) {
                            for (std::int32_t c = 0; c < kChannels; ++c) {
                                interleaved[static_cast<std::size_t>(i * kChannels + c)] =
                                    mixed->channel(c)[i];
                            }
                        }
                    }
                }
                audioWritten_ += sink_->ring().write(interleaved.data(), block);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    void followClock() {
        if (!playing_) {
            return;
        }
        const time::Rational& audioRate = liveSequence()->audioSampleRate();
        const std::int64_t clock = sink_ ? sink_->clockFrames() : 0;

        // Position is an exact rational function of elapsed audio, floored to
        // the frame the playhead is inside -- the same arithmetic the scheduler
        // uses, and for the same reason.
        const time::Rational elapsed = time::Rational{clock - anchorClock_, 1} / audioRate;
        const time::Rational advanced = elapsed * transport_.speed() * liveSequence()->frameRate();
        const auto position = anchorPosition_ + time::RationalTime{advanced.floorToInt(),
                                                                   liveSequence()->frameRate()};

        if (position.frames() >= liveSequence()->duration().frames() || position.frames() < 0) {
            stop();
            return;
        }
        setPosition(position);
    }

    /// Measure the frame at the playhead, if anyone is looking at the scopes.
    ///
    /// Not during playback, and not when the panel is hidden. Measuring means
    /// compositing a frame on the CPU, and doing that on every frame of
    /// playback would spend more time on the instrument than on the picture --
    /// the readback the GPU path exists to avoid, reintroduced through the
    /// back door.
    ///
    /// It composites through render::RenderGraph rather than reading back the
    /// preview, so the scope measures what will be delivered. That is the
    /// number a grade is judged against, and it does not make playback pay for
    /// a readback it otherwise does not need.
    /// Composite the frame under the playhead, and give it to whatever wants it.
    ///
    /// One render for both instruments. The scopes measure it and the Audio
    /// workspace's thumbnail shows it, and they are never both up -- but
    /// compositing twice for the one that is would be paying for the frame
    /// twice, and the frame is the expensive part.
    ///
    /// Not while playing. Both of these are things somebody looks at when
    /// stopped, and a composite per displayed frame is the transport's budget
    /// spent on a picture that is already on screen.
    void refreshInstruments() {
        if (media_ == nullptr || liveSequence() == nullptr) {
            return;
        }
        const bool wantScopes = scopes_ != nullptr && scopes_->wantsMeasurement();
        const bool wantThumb = thumb_ != nullptr && thumb_->isVisible();
        if (playing_ || (!wantScopes && !wantThumb)) {
            return;
        }
        render::RenderGraph graph{*media_};
        graph.setTextRasterizer(&text_);
        graph.setProject(&project_);
        auto frame = graph.composite(*liveSequence(), position_);
        if (!frame) {
            if (wantScopes) {
                scopes_->clear();
            }
            if (wantThumb) {
                thumb_->clearFrame();
            }
            return;
        }

        if (wantScopes) {
            render::ScopeOptions options;
            options.waveformColumns = std::max(64, scopes_->width());
            // Every second row. The shape of a waveform does not change for
            // being measured at half the vertical resolution, and this is
            // running between scrubs.
            options.rowStride = 2;
            scopes_->setScopes(render::measure(*frame, options));
        }
        if (wantThumb) {
            showThumbnail(*frame);
        }
    }

    /// The composited frame, encoded for display and handed to the thumbnail.
    void showThumbnail(const render::RgbaImage& frame) {
        const int wide = frame.width();
        const int tall = frame.height();
        if (wide <= 0 || tall <= 0) {
            thumb_->clearFrame();
            return;
        }
        const int stride = wide * 3;
        std::vector<std::uint8_t> rgb(static_cast<std::size_t>(stride) *
                                      static_cast<std::size_t>(tall));
        if (!render::toDisplayRgb24(frame, rgb.data(), stride)) {
            thumb_->clearFrame();
            return;
        }
        // Copied, because the QImage above only borrows the vector, and the
        // vector is gone at the end of this function.
        thumb_->setFrame(QImage{rgb.data(), wide, tall, stride, QImage::Format_RGB888}.copy());

        const model::Sequence* sequence = liveSequence();
        const model::Track* track = sequence != nullptr && !sequence->videoTracks().empty()
                                        ? &sequence->videoTracks().front()
                                        : nullptr;
        const model::Clip* clip = track != nullptr ? track->clipAt(position_) : nullptr;
        const bool dropFrame =
            sequence != nullptr && time::supportsDropFrame(sequence->frameRate());
        thumb_->setCaption(
            clip != nullptr ? QString::fromStdString(clip->name) : QString{"—"},
            sequence != nullptr
                ? QString::fromStdString(
                      time::timecodeFromFrames(position_.frames(), sequence->frameRate(), dropFrame)
                          .toString())
                : QString{});
    }

    /// Measure the whole programme's loudness.
    ///
    /// On demand, not continuously: the reading that matters is the gated
    /// integrated figure over everything, and that means mixing the sequence
    /// from end to end. Doing it every time a fader moved would make the mixer
    /// unusable to pay for a number nobody reads until delivery.
    void measureProgramme() {
        const model::Sequence* sequence = liveSequence();
        if (sequence == nullptr) {
            return;
        }
        QApplication::setOverrideCursor(Qt::WaitCursor);
        render::AudioGraph graph{*media_};
        const time::TimeRange whole{time::RationalTime{0, sequence->frameRate()},
                                    sequence->duration()};
        auto measured = graph.measureLoudness(*sequence, whole);
        QApplication::restoreOverrideCursor();
        if (!measured) {
            app::warn(this, "Loudness", QString::fromStdString(measured.error().message()));
            return;
        }
        loudness_->setMeasurement(*measured);
    }

    /// Say which stages of the chain this shot has been through.
    ///
    /// Read from the clip rather than remembered, for the same reason the
    /// panels are: after an undo the clip is the only thing that knows.
    void refreshGradeChain() {
        const model::Sequence* sequence = liveSequence();
        const model::Track* track =
            sequence != nullptr ? sequence->findTrack(selectedTrack_) : nullptr;
        const model::Clip* clip = track != nullptr ? track->find(selectedClip_) : nullptr;
        nodes_->setEnabledChain(clip != nullptr);
        if (clip == nullptr) {
            nodes_->setOccupied({false, false, false, false});
            return;
        }
        nodes_->setOccupied({!clip->color.isIdentity() || !clip->wheels.isIdentity(),
                             !clip->curves.isIdentity(), clip->secondary.qualifier.enabled,
                             !clip->lut.path.empty()});
    }

    /// Keep the frame that is on screen, as a reference to grade against.
    ///
    /// Grabbed off the monitor rather than composited again: what somebody
    /// means by "this frame" is the one they are looking at, and rendering a
    /// second one would be a different picture the moment anything about the
    /// grade or the proxy setting differed.
    void grabStill() {
        const QImage shot = monitor_->grab().toImage();
        if (shot.isNull()) {
            return;
        }
        const model::Sequence* sequence = liveSequence();
        const bool dropFrame =
            sequence != nullptr && time::supportsDropFrame(sequence->frameRate());
        const QString name =
            sequence != nullptr
                ? QString::fromStdString(
                      time::timecodeFromFrames(position_.frames(), sequence->frameRate(), dropFrame)
                          .toString())
                : QString{"still"};
        gallery_->addStill(shot, position_, name);
    }

    /// Put a .cube on the selected shot.
    void applyLookToSelection(const QString& path) {
        if (!selectedClip_.isValid()) {
            app::say(this, "Look", "Pick a shot first — a look goes on a clip.");
            return;
        }
        model::LutRef look;
        look.path = path.toStdString();
        look.amount = 1.0;
        auto built = edit::makeSetLut(project_, {sequenceId_, selectedTrack_}, selectedClip_, look);
        if (!built) {
            return;
        }
        commands_.execute(project_, std::move(*built));
        commands_.breakMerge();
        renderCache_.clear();
        effects_->refresh();
        clipStrip_->refresh();
        refreshGradeChain();
        monitor_->update();
        refreshInstruments();
        updateTitle();
    }

    /// Attach proxies, make them, and switch between them and the originals.
    void proxyMenu() {
        QMenu menu;
        QAction* toggle = menu.addAction("Use proxies");
        toggle->setCheckable(true);
        toggle->setChecked(project_.usingProxies());

        std::size_t proxied = 0;
        for (const model::MediaRef& media : project_.media()) {
            proxied += media.proxyPath.empty() ? 0 : 1;
        }
        toggle->setEnabled(proxied > 0);
        menu.addSeparator();
        menu.addAction(QString("%1 of %2 have proxies").arg(proxied).arg(project_.media().size()))
            ->setEnabled(false);
        menu.addSeparator();

        // One entry per media reference, so a file can be given its proxy
        // without a second panel to manage.
        std::map<QAction*, model::MediaRefId> attach;
        std::map<QAction*, model::MediaRefId> build;
        for (const model::MediaRef& media : project_.media()) {
            QAction* action = menu.addAction(
                QString("Attach proxy for %1…").arg(QString::fromStdString(media.name)));
            attach.emplace(action, media.id);
        }
        menu.addSeparator();
        for (const model::MediaRef& media : project_.media()) {
            if (!media.proxyPath.empty()) {
                continue;  // it has one; making a second is not a thing to offer
            }
            QAction* action = menu.addAction(
                QString("Make a proxy for %1").arg(QString::fromStdString(media.name)));
            build.emplace(action, media.id);
        }

        QAction* chosen = menu.exec(QCursor::pos());
        if (chosen == nullptr) {
            return;
        }
        if (chosen == toggle) {
            project_.setUsingProxies(toggle->isChecked());
            // The media source resolved its paths when it opened, so switching
            // means reopening. Cheaper than deciding per read, and it is the
            // only moment the decision changes.
            if (Status reopened = openMedia(); !reopened) {
                app::warn(this, "Proxies", QString::fromStdString(reopened.error().toString()));
            }
            monitor_->update();
            refresh();
            return;
        }
        if (const auto making = build.find(chosen); making != build.end()) {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            auto made = buildProxy(making->second);
            QApplication::restoreOverrideCursor();
            if (!made) {
                app::warn(this, "Proxies", QString::fromStdString(made.error().message()));
                return;
            }
            app::say(
                this, "Proxies",
                QString("Made a %1x%2 proxy of %3 frames, %4% of the size.")
                    .arg(made->width)
                    .arg(made->height)
                    .arg(made->frames)
                    .arg(made->sourceBytes == 0 ? 0.0
                                                : (100.0 * static_cast<double>(made->proxyBytes) /
                                                   static_cast<double>(made->sourceBytes)),
                         0, 'f', 1));
            return;
        }
        const auto found = attach.find(chosen);
        if (found == attach.end()) {
            return;
        }
        const QString path = QFileDialog::getOpenFileName(this, "Choose a proxy file");
        if (path.isEmpty()) {
            return;
        }
        for (model::MediaRef& media : project_.mediaMutable()) {
            if (media.id == found->second) {
                media.proxyPath = path.toStdString();
            }
        }
    }

    /// Work out the offsets between a multicam clip's angles.
    ///
    /// Two methods, because a shoot is either jam-synced or it is not, and
    /// there is no useful middle: timecode is exact when it is there, and by
    /// ear is what is left when it is not.
    void multicamMenu() {
        const model::Sequence* sequence = liveSequence();
        if (sequence == nullptr) {
            return;
        }
        const model::Track* track = sequence->findTrack(selectedTrack_);
        const model::Clip* clip = track != nullptr ? track->find(selectedClip_) : nullptr;

        QMenu menu;
        QAction* byTimecode = menu.addAction("Sync angles by timecode");
        QAction* byAudio = menu.addAction("Sync angles by audio");
        const bool multicam = clip != nullptr && clip->isMulticam();
        byTimecode->setEnabled(multicam);
        byAudio->setEnabled(multicam && media_ != nullptr);
        if (!multicam) {
            menu.addSeparator();
            menu.addAction("Select a multicam clip first")->setEnabled(false);
        }

        QAction* chosen = menu.exec(QCursor::pos());
        if (chosen == nullptr || !multicam) {
            return;
        }
        syncAngles(chosen == byAudio);
    }

    /// Pre-render, so a stack the CPU has to composite plays back.
    ///
    /// The visible range rather than the whole sequence: what somebody wants
    /// rendered is what they are about to watch, and "render everything" on a
    /// long timeline is a decision to wait for frames nobody asked about. The
    /// range is chosen by scrolling and zooming, which they are doing anyway.
    void renderMenu() {
        const model::Sequence* sequence = liveSequence();
        if (sequence == nullptr || media_ == nullptr) {
            return;
        }
        const time::TimeRange visible = timeline_->layout().visibleRange(sequence->frameRate());

        QMenu menu;
        QAction* render = menu.addAction(
            QString("Render the visible range (%1 frames)").arg(visible.duration().frames()));
        render->setEnabled(!visible.isEmpty());
        QAction* clear = menu.addAction("Clear the render cache");
        menu.addSeparator();
        menu.addAction(QString("%1 frames cached, %2 MB")
                           .arg(renderCache_.count())
                           .arg(renderCache_.byteSize() / (1024 * 1024)))
            ->setEnabled(false);

        QAction* chosen = menu.exec(QCursor::pos());
        if (chosen == clear) {
            renderCache_.clear();
            updateCacheBar();
            return;
        }
        if (chosen != render) {
            return;
        }
        renderVisibleRange();
    }

    /// Measure the programme, and offer to normalise it.
    ///
    /// The measurement is of the whole sequence through the real mix, so it
    /// takes a moment on a long one — which is why it is a menu action rather
    /// than a meter that runs continuously. Loudness is a delivery check, done
    /// once near the end, not something to watch while cutting.
    void loudnessMenu() {
        if (liveSequence() == nullptr || media_ == nullptr) {
            return;
        }
        render::AudioGraph mixer{*media_};
        const time::TimeRange whole{time::RationalTime{0, liveSequence()->frameRate()},
                                    liveSequence()->duration()};
        auto measured = mixer.measureLoudness(*liveSequence(), whole);
        if (!measured) {
            app::warn(this, "Loudness", QString::fromStdString(measured.error().toString()));
            return;
        }

        constexpr double kTarget = -23.0;  // EBU R128
        const double gain = measured->gainToReach(kTarget);

        QMenu menu;
        menu.addAction(QString("Integrated: %1 LUFS").arg(measured->integratedLufs, 0, 'f', 1))
            ->setEnabled(false);
        menu.addAction(QString("Sample peak: %1 dBFS").arg(measured->samplePeakDbfs, 0, 'f', 1))
            ->setEnabled(false);
        menu.addSeparator();
        QAction* normalise = menu.addAction(QString("Normalise to %1 LUFS (%2%3 dB)")
                                                .arg(kTarget, 0, 'f', 0)
                                                .arg(gain >= 0.0 ? "+" : "")
                                                .arg(gain, 0, 'f', 1));
        // Nothing to do to silence, and nothing worth doing for a tenth of a
        // decibel.
        normalise->setEnabled(std::fabs(gain) > 0.1);

        if (menu.exec(QCursor::pos()) != normalise) {
            return;
        }
        // Applied to every audio track's fader rather than to a master gain,
        // which does not exist: the balance between tracks is a decision
        // somebody made, and moving them all by the same amount keeps it.
        for (const model::Track& track : liveSequence()->audioTracks()) {
            edit::TrackState state;
            state.muted = track.isMuted();
            state.soloed = track.isSoloed();
            state.gainDb = track.gainDb() + gain;
            state.pan = track.pan();
            if (auto built =
                    edit::makeSetTrackState(project_, liveSequence()->id(), track.id(), state)) {
                commands_.execute(project_, std::move(*built));
            }
        }
        commands_.breakMerge();
        mixer_->refresh();
    }

    /// Import, export and burn-in, from one menu.
    ///
    /// A menu rather than a panel: captions are imported once, exported once,
    /// and otherwise left alone, and a permanent panel for three actions would
    /// take room from the ones used constantly.
    void captionsMenu() {
        if (liveSequence() == nullptr) {
            return;
        }
        QMenu menu;
        QAction* importAction = menu.addAction("Import subtitles…");
        QAction* exportAction = menu.addAction("Export subtitles…");
        menu.addSeparator();
        QAction* burnAction = menu.addAction("Burn in");
        burnAction->setCheckable(true);
        burnAction->setChecked(liveSequence()->captions().isBurnedIn());

        const std::size_t count = liveSequence()->captions().size();
        exportAction->setEnabled(count > 0);
        burnAction->setEnabled(count > 0);
        menu.addSeparator();
        menu.addAction(QString("%1 captions").arg(count))->setEnabled(false);

        QAction* chosen = menu.exec(QCursor::pos());
        if (chosen == nullptr) {
            return;
        }
        if (chosen == importAction) {
            const QString path = QFileDialog::getOpenFileName(
                this, "Open subtitles", {}, "Subtitles (*.srt *.vtt);;All files (*)");
            if (path.isEmpty()) {
                return;
            }
            auto loaded = io::loadSubtitles(path.toStdString());
            if (!loaded) {
                app::warn(this, "Subtitles", QString::fromStdString(loaded.error().toString()));
                return;
            }
            // The style and the burn-in setting belong to the sequence, not to
            // the file: importing a new script should not silently turn burn-in
            // off or lose a typeface somebody chose.
            model::CaptionTrack merged = *loaded;
            merged.setStyle(liveSequence()->captions().style());
            merged.setBurnedIn(liveSequence()->captions().isBurnedIn());
            applyCaptions(merged);
            return;
        }
        if (chosen == exportAction) {
            const QString path = QFileDialog::getSaveFileName(
                this, "Save subtitles", "captions.srt", "SubRip (*.srt);;WebVTT (*.vtt)");
            if (path.isEmpty()) {
                return;
            }
            const auto format = io::formatForPath(path.toStdString());
            if (Status saved =
                    io::saveSubtitles(liveSequence()->captions(), path.toStdString(), format);
                !saved) {
                app::warn(this, "Subtitles", QString::fromStdString(saved.error().toString()));
            }
            return;
        }
        if (chosen == burnAction) {
            model::CaptionTrack changed = liveSequence()->captions();
            changed.setBurnedIn(burnAction->isChecked());
            applyCaptions(changed);
        }
    }

    void applyCaptions(const model::CaptionTrack& captions) {
        auto built = edit::makeSetCaptions(project_, liveSequence()->id(), captions);
        if (!built) {
            return;
        }
        commands_.execute(project_, std::move(*built));
        commands_.breakMerge();
        monitor_->update();
        timeline_->update();
        refreshInstruments();
    }

    void refresh() {
        if (liveSequence() == nullptr) {
            return;
        }
        const bool dropFrame = time::supportsDropFrame(liveSequence()->frameRate());
        const time::Timecode code =
            time::timecodeFromFrames(position_.frames(), liveSequence()->frameRate(), dropFrame);
        timecode_->setText(QString::fromStdString(code.toString()));
        if (!scrubber_->isSliderDown()) {
            scrubber_->setValue(static_cast<int>(position_.frames()));
        }

        if (deliver_ != nullptr) {
            deliver_->setPlayhead(position_);
        }

        const time::Timecode left = time::timecodeFromFrames(
            std::max<std::int64_t>(0, liveSequence()->duration().frames() - position_.frames()),
            liveSequence()->frameRate(), dropFrame);
        remaining_->setText("-" + QString::fromStdString(left.toString()));

        const model::Track* track = liveSequence()->findTrack(selectedTrack_);
        const model::Clip* clip = track != nullptr ? track->find(selectedClip_) : nullptr;
        viewerOverlay_->setInfo(
            clip != nullptr ? QString::fromStdString(clip->name) : QString{},
            QString::fromStdString(code.toString()),
            QString("%1×%2").arg(liveSequence()->width()).arg(liveSequence()->height()),
            track != nullptr ? QString::fromStdString(track->name()) : QString{});
    }

    model::Project project_;
    io::LoadedProject loaded_;
    /// Where the project came from, and where Save writes.
    std::string path_;
    QTimer* autosaveTimer_{nullptr};
    /// The active sequence, by id rather than by pointer.
    ///
    /// A pointer into the project's vector of sequences is valid until one is
    /// added, and adding one is now an ordinary thing to do -- making a
    /// sequence to nest is how nesting starts. The dangling pointer presented
    /// as a zero denominator deep inside rational arithmetic, which is a long
    /// way from the cause.
    model::SequenceId sequenceId_;
    std::unique_ptr<platform::ffmpeg::ProjectMediaSource> media_;
    /// One font engine for the window: the preview, the scopes and the export
    /// dialog all draw the same titles.
    platform::qtext::QtTextRasterizer text_;
    std::unique_ptr<platform::sdl::AudioSink> sink_;
    /// Composited frames, shared between the preview's CPU fallback and the
    /// pre-render. One cache: a frame rendered by the button is the frame the
    /// monitor asks for a moment later, and two caches would render it twice.
    render::RenderCache renderCache_;
    /// Comparison view: a way of looking, not part of the cut, so none of this
    /// is saved with the project.
    bool comparing_{false};
    time::RationalTime referenceAt_{};
    render::CompareMode compareMode_{render::CompareMode::Split};
    double compareSplit_{0.5};
    /// What the timeline last said was picked. Multicam syncing acts on one
    /// clip, and this is which one.
    model::TrackId selectedTrack_;
    model::ClipId selectedClip_;
    std::int32_t lastSyncCount_{0};
    std::int32_t lastSyncSkipped_{0};

    app::ProgramMonitor* monitor_{nullptr};
    app::MaskOverlay* maskOverlay_{nullptr};
    app::TimelineWidget* timeline_{nullptr};
    app::EffectControls* effects_{nullptr};
    app::ScopesPanel* scopes_{nullptr};
    app::MixerPanel* mixer_{nullptr};
    std::mutex meterMutex_;
    render::AudioGraph::Meters latestMeters_;
    QTimer* meterTimer_{nullptr};
    app::ProjectBin* bin_{nullptr};
    app::GalleryPanel* gallery_{nullptr};
    app::LoudnessPanel* loudness_{nullptr};
    app::FrameThumb* thumb_{nullptr};
    app::StemsPanel* stems_{nullptr};
    app::ChannelPanel* channel_{nullptr};
    QWidget* audioSide_{nullptr};
    QWidget* viewerBar_{nullptr};
    app::ClipStrip* clipStrip_{nullptr};
    app::GradeNodes* nodes_{nullptr};
    app::ColorPalette* palette_{nullptr};
    QWidget* nodesBox_{nullptr};
    QWidget* timelinePane_{nullptr};
    app::MediaBrowser* browser_{nullptr};
    app::Transcript* transcript_{nullptr};
    app::Hotkeys* hotkeys_{nullptr};
    static inline QString keymapPath_;

    /// Which keystroke runs what, and everything that can be run.
    ui::Keymap keymap_;
    QMap<QString, QAction*> actions_;
    std::map<std::string, std::function<void()>> handlers_;
    /// Somebody else has this project open, so it must not be written over.
    bool readOnly_{false};
    static inline bool locking_{false};
    app::SourceMonitor* source_{nullptr};
    QSplitter* topSplitter_{nullptr};
    QSplitter* mainSplitter_{nullptr};

    /// The chrome. None of it owns anything: every one of these is a child of
    /// the window, and Qt deletes them with it.
    QMenuBar* menuBar_{nullptr};
    QWidget* viewerWell_{nullptr};
    QLabel* noMonitorLabel_{nullptr};
    bool sourceShown_{false};
    bool programShown_{true};
    QStackedWidget* workspaceStack_{nullptr};
    QStackedWidget* actionStack_{nullptr};
    app::DeliverPanel* deliver_{nullptr};
    QPushButton* renderButton_{nullptr};
    app::ViewerOverlay* viewerOverlay_{nullptr};
    QLabel* projectLabel_{nullptr};
    QLabel* autosaveLabel_{nullptr};
    QLabel* formatLabel_{nullptr};
    QLabel* viewerLabel_{nullptr};
    QLabel* qualityLabel_{nullptr};
    QLabel* timelineLabel_{nullptr};
    QLabel* snapLabel_{nullptr};
    QLabel* statusLeft_{nullptr};
    QLabel* statusMiddle_{nullptr};
    QLabel* statusRight_{nullptr};
    QLabel* remaining_{nullptr};
    QSlider* zoomSlider_{nullptr};
    QPushButton* sourceTab_{nullptr};
    QPushButton* programTab_{nullptr};
    QPushButton* snapButton_{nullptr};
    QPushButton* guidesButton_{nullptr};
    QAction* guidesAction_{nullptr};
    QAction* compareAction_{nullptr};
    std::vector<QPushButton*> toolButtons_;
    QMap<QString, QPushButton*> workspaceTabs_;
    QMap<QString, QAction*> workspaceActions_;
    QString workspace_{"Edit"};
    edit::CommandStack commands_;
    QLabel* timecode_{nullptr};
    QPushButton* playButton_{nullptr};
    QSlider* scrubber_{nullptr};
    QTimer* clockTimer_{nullptr};

    playback::Transport transport_;
    time::RationalTime position_{};
    time::RationalTime anchorPosition_{};
    std::int64_t anchorClock_{0};
    std::int64_t audioWritten_{0};
    bool playing_{false};

    std::thread audioThread_;
    std::thread waveformThread_;
    std::atomic<bool> shuttingDown_{false};
    std::atomic<bool> audioRunning_{false};
};

}  // namespace zaro::app
