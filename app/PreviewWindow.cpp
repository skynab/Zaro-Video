// The preview window's construction and its dialogs.
//
// Two things live here, and they are here for the same reason. The constructor
// was four hundred and seventy-seven lines: every panel the window owns, the
// splitters they sit in, and the hundred-odd connections between them, in one
// function that grew by a dozen lines every time a panel was added. It is now
// six, named for the phases it already had -- make the panels, lay out the
// viewer, wire the workspace, lay out the window, wire the editing, start the
// clocks -- in that order, because the order was never arbitrary: a layout
// cannot add a widget that does not exist yet, and a connection cannot name a
// panel that has not been made.
//
// The dialogs are the other half. Each is the same shape -- ask for a path or
// a choice, call into commands:: or edit::, say what happened -- and none of
// them is about the window beyond the fact that a dialog needs a parent. They
// were in the header only because they were methods, and they were methods
// only because that is where the state they read happens to be kept.
//
// What stays in the header is what a caller needs to see: the declarations,
// and the small forwarding methods that are easier to read than to find.

#include "PreviewWindow.h"

#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace zaro::app {

PreviewWindow::PreviewWindow(model::Project project, io::LoadedProject loaded, std::string path) {
    // What was just loaded is what is on disk, by definition, and adopt
    // marks it so.
    document_.adopt(std::move(project), std::move(loaded), std::move(path));
    sequenceId_ = document_.project().activeSequence();

    createPanels();

    // Everything the window can do lives in a menu. It used to live in a
    // row of eighteen buttons under the picture, which was fine while there
    // were four of them: a menu bar is the structure that says which of
    // them belong together, and it costs the transport nothing.
    // The keymap first: menus read their shortcuts from it as they are
    // built, so a customised binding is on the item the first time it is
    // drawn rather than after a refresh nobody triggers.
    loadKeymap();
    // Every command says what it does before anything shows one: the menus
    // and the bars ask the router for an action by id, and an id nothing is
    // bound to is a menu item that does nothing.
    bindCommands();
    bindPlaybackActions();
    buildMenus();

    buildViewerLayout();
    wireWorkspacePanels();
    buildWindowLayout();
    wireEditingSignals();
    startTimers();
}

void PreviewWindow::createPanels() {
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

    bars_.timecode = new QLabel(this);
    bars_.timecode->setObjectName("timecode-big");
    // Ask the system for its fixed-width family rather than naming one:
    // a missing family costs a slow alias lookup and silently falls back.
    QFont monospace = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monospace.setPointSize(17);
    bars_.timecode->setFont(monospace);
    bars_.remaining = new QLabel(this);
    QFont smallMonospace = monospace;
    smallMonospace.setPointSize(11);
    bars_.remaining->setFont(smallMonospace);
    bars_.remaining->setProperty("muted", true);
    bars_.playButton = new QPushButton(kPlayGlyph, this);
    bars_.playButton->setToolTip("Play or pause (Space)");
    bars_.playButton->setProperty("accent", true);
    bars_.playButton->setFixedSize(46, 30);
    bars_.scrubber = new QSlider(Qt::Horizontal, this);

    timeline_ = adopting(new app::TimelineWidget(this));
    effects_ = adopting(new app::EffectControls(this));
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
    mixer_ = adopting(new app::MixerPanel(this));
    // 250 was under what the parameter rows actually measure, so the
    // splitter was free to squeeze the column until the value fields ran
    // off the edge of it. Now it is the width the content needs.
    effects_->setMinimumWidth(300);
    effects_->setMaximumWidth(330);

    // Monitor and parameters side by side, transport under them, timeline
    // across the bottom.
    bin_ = adopting(new app::ProjectBin(this));
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
    stems_ = adopting(new app::StemsPanel(this));
    channel_ = adopting(new app::ChannelPanel(this));
    channel_->setMinimumWidth(280);
    channel_->setMaximumWidth(320);

    gallery_ = new app::GalleryPanel(this);
    gallery_->setFixedWidth(236);
    clipStrip_ = adopting(new app::ClipStrip(this));
    nodes_ = new app::GradeNodes(this);
    palette_ = adopting(new app::ColorPalette(this));
    palette_->setFixedHeight(212);

    source_ = new app::SourceMonitor(this);
    source_->setMinimumWidth(280);
}

void PreviewWindow::buildViewerLayout() {
    // Source and program side by side, each shown or not on its own.
    //
    // Two toggles rather than two tabs, as the design draws them: comparing
    // the clip you are about to place against the cut you are placing it
    // into is the whole reason both monitors exist, and a stack can only
    // ever answer "which one" -- never "both". Either can be off, including
    // both, because a window given over to the timeline is a real way to
    // work and the well is the largest thing to reclaim.
    bars_.viewerWell = new QWidget(this);
    bars_.viewerWell->setObjectName("viewer-well");
    bars_.viewerWell->setAttribute(Qt::WA_StyledBackground, true);
    auto* wellRow = new QHBoxLayout(bars_.viewerWell);
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
    bars_.noMonitorLabel = new QLabel("No monitor shown \u2014 turn on Source or Program", this);
    bars_.noMonitorLabel->setAlignment(Qt::AlignCenter);
    bars_.noMonitorLabel->setProperty("muted", true);
    wellRow->addWidget(bars_.noMonitorLabel, 1);

    auto* programColumn = new QWidget(this);
    auto* programLayout = new QVBoxLayout(programColumn);
    programLayout->setContentsMargins(0, 0, 0, 0);
    programLayout->setSpacing(0);
    bars_.viewerBar = buildViewerBar();
    programLayout->addWidget(bars_.viewerBar);
    programLayout->addWidget(bars_.viewerWell, 1);
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
    bars_.audioSide = leftColumn;

    topSplitter_->addWidget(bars_.audioSide);
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
    bars_.nodesBox = new QWidget(gradeColumn);
    bars_.nodesBox->setObjectName("grade-nodes-box");
    auto* nodesLayout = new QVBoxLayout(bars_.nodesBox);
    nodesLayout->setContentsMargins(12, 10, 12, 10);
    nodesLayout->addWidget(nodes_);
    gradeLayout->addWidget(bars_.nodesBox);
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
}

void PreviewWindow::wireWorkspacePanels() {
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
    connect(loudness_, &app::LoudnessPanel::measureRequested, this, [this] { measureProgramme(); });

    // Picking a stem is how somebody asks "where is the music": it goes to
    // the first clip carrying that role and selects it, so the mixer and
    // the channel panel are looking at the same sound.
    connect(
        stems_, &app::StemsPanel::stemChosen, this,
        [this](zaro::model::TrackId track, zaro::model::ClipId clip, zaro::time::RationalTime at) {
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
    connect(gallery_, &app::GalleryPanel::stillChosen, this, [this](zaro::time::RationalTime at) {
        setCompareMode(render::CompareMode::Split);
        setComparing(true, at);
        monitor_->update();
    });
    connect(gallery_, &app::GalleryPanel::lutChosen, this,
            [this](const QString& lut) { applyLookToSelection(lut); });

    connect(bin_, &app::ProjectBin::openRequested, this, [this](zaro::model::MediaRefId id) {
        if (const model::MediaRef* ref = document_.project().findMedia(id)) {
            source_->load(*ref);
            // Opening a clip is a request to look at it.
            setSourceShown(true);
        }
    });
    connect(bin_, &app::ProjectBin::openSubclipRequested, this, [this](zaro::model::SubclipId id) {
        const model::Subclip* subclip = document_.project().findSubclip(id);
        if (subclip == nullptr) {
            return;
        }
        if (const model::MediaRef* ref = document_.project().findMedia(subclip->source)) {
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
        bars_.scrubber->setRange(0, static_cast<int>(liveSequence()->duration().frames()));
        timeline_->update();
        monitor_->update();
        refresh();
    });
}

void PreviewWindow::buildWindowLayout() {
    mainSplitter_ = new QSplitter(Qt::Vertical, this);
    mainSplitter_->setHandleWidth(1);
    mainSplitter_->addWidget(topSplitter_);
    bars_.timelinePane = buildTimelinePane();
    mainSplitter_->addWidget(bars_.timelinePane);
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
    deliver_ = adopting(new app::DeliverPanel(this));
    connect(deliver_, &app::DeliverPanel::queueChanged, this, [this] { updateChrome(); });

    bars_.workspaceStack = new QStackedWidget(this);
    bars_.workspaceStack->addWidget(mainSplitter_);
    bars_.workspaceStack->addWidget(deliver_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(buildTitleBar());
    layout->addWidget(buildToolBar());
    layout->addWidget(bars_.workspaceStack, 1);
    layout->addWidget(buildStatusBar());
}

void PreviewWindow::wireEditingSignals() {
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
                maskOverlay_->setTarget(&document_.project(), sequenceId_, track, clip,
                                        &document_.commands());
            });
    connect(scopes_, &app::ScopesPanel::measurementNeeded, this, [this] { refreshInstruments(); });
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
        bars_.scrubber->setRange(0, static_cast<int>(liveSequence()->duration().frames()));
        monitor_->update();
        refresh();
        // An edit invalidates whatever it shows on, so the bar has to be
        // re-read rather than left claiming a range that no longer renders
        // to what is cached.
        updateCacheBar();
        updateTitle();
    });

    connect(bars_.playButton, &QPushButton::clicked, this, [this] { togglePlay(); });
    connect(bars_.scrubber, &QSlider::sliderMoved, this, [this](int value) {
        // Scrubbing stops playback: the playhead is being driven by hand,
        // and having the clock fight it is what makes scrubbing feel loose.
        stop();
        setPosition(time::RationalTime{value, liveSequence()->frameRate()});
    });

    // The clock drives the playhead, and the play button follows the clock
    // rather than the click: pressing play when the device will not open
    // leaves the transport stopped, and a button that already said "pause"
    // would be describing something that is not happening.
    connect(&playback_, &PlaybackController::moved, this,
            [this](const time::RationalTime& at) { setPosition(at); });
    connect(&playback_, &PlaybackController::playingChanged, this, [this](bool playing) {
        if (bars_.playButton != nullptr) {
            bars_.playButton->setText(playing ? kPauseGlyph : kPlayGlyph);
        }
    });
}

void PreviewWindow::startTimers() {
    // Meters at twenty a second. Faster is invisible on a meter with a peak
    // hold, and each tick is a mix of a short block when the transport is
    // stopped.
    meterTimer_ = new QTimer(this);
    meterTimer_->setInterval(50);
    connect(meterTimer_, &QTimer::timeout, this, [this] { updateMeters(); });
    meterTimer_->start();

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

void PreviewWindow::exportDialog() {
    if (liveSequence() == nullptr) {
        return;
    }
    stop();
    app::ExportDialog dialog{document_.project(), liveSequence()->id(), this};
    dialog.exec();
}

void PreviewWindow::exportOtio() {
    if (liveSequence() == nullptr) {
        return;
    }
    // Export only, from the window. Importing an OTIO file produces a
    // project of its own, and replacing the open one needs a "save first?"
    // that does not exist yet -- so that direction lives in zaro-otio,
    // where there is nothing to lose.
    const QString path = QFileDialog::getSaveFileName(this, "Export OpenTimelineIO",
                                                      "timeline.otio", "OpenTimelineIO (*.otio)");
    if (path.isEmpty()) {
        return;
    }
    if (Status saved = io::saveOtio(document_.project(), liveSequence()->id(), path.toStdString());
        !saved) {
        app::warn(this, "OpenTimelineIO", QString::fromStdString(saved.error().toString()));
    }
}

void PreviewWindow::trackMask() {
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

void PreviewWindow::stabilise() {
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

void PreviewWindow::clearStabilisation() {
    const model::Sequence* sequence = liveSequence();
    if (sequence == nullptr || !selectedClip_.isValid()) {
        return;
    }
    auto built = edit::makeStabilise(document_.project(), {sequence->id(), selectedTrack_},
                                     selectedClip_, model::Curve{}, model::Curve{}, 1.0);
    if (!built) {
        return;
    }
    document_.commands().execute(document_.project(), std::move(*built));
    document_.commands().breakMerge();
    renderCache_.clear();
    monitor_->update();
    effects_->refresh();
    updateCacheBar();
    updateTitle();
}

void PreviewWindow::relinkDialog() {
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
    QString said = QString("Relinked %1 of %2 missing file%3, from %4 looked at.")
                       .arg(report->matches.size())
                       .arg(report->matches.size() + report->stillMissing.size())
                       .arg(report->matches.size() + report->stillMissing.size() == 1 ? "" : "s")
                       .arg(report->examined);
    if (byName > 0) {
        // Said plainly: a file matched only by its name is one somebody
        // should look at before trusting the cut.
        said += QString("\n%1 matched by name only -- check they are the right takes.").arg(byName);
    }
    app::say(this, "Relink", said);
}

void PreviewWindow::consolidateDialog() {
    const QString into = QFileDialog::getExistingDirectory(this, "Gather the project's media into");
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
        said +=
            QString("\n%1 could not be found -- relink them first.").arg(report->missing.size());
    }
    app::say(this, "Consolidate", said);
}

void PreviewWindow::saveTemplateDialog() {
    const QString path = QFileDialog::getSaveFileName(this, "Save graphic as template", {},
                                                      "Graphic template (*.zarograph)");
    if (path.isEmpty()) {
        return;
    }
    if (Status saved = saveGraphicTemplate(path.toStdString()); !saved) {
        app::warn(this, "Template", QString::fromStdString(saved.error().message()));
    }
}

void PreviewWindow::placeTemplateDialog() {
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

void PreviewWindow::exportReviewNotes() {
    const QString chosen =
        QFileDialog::getSaveFileName(this, "Export review notes", {}, "Markdown (*.md)");
    if (chosen.isEmpty()) {
        return;
    }
    if (Status written = writeReviewNotes(chosen.toStdString()); !written) {
        app::warn(this, "Review", QString::fromStdString(written.error().message()));
    }
}

void PreviewWindow::matchShot() {
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

void PreviewWindow::proxyMenu() {
    std::vector<chrome::ProxyEntry> entries;
    entries.reserve(document_.project().media().size());
    for (const model::MediaRef& media : document_.project().media()) {
        entries.push_back({media.id, QString::fromStdString(media.name), !media.proxyPath.empty()});
    }
    const chrome::ProxyChoice chosen =
        chrome::proxyMenu(entries, document_.project().usingProxies());
    switch (chosen.kind) {
        case chrome::ProxyChoice::Kind::ToggleUsingProxies: {
            document_.project().setUsingProxies(!document_.project().usingProxies());
            // The media source resolved its paths when it opened, so
            // switching means reopening. Cheaper than deciding per read,
            // and it is the only moment the decision changes.
            if (Status reopened = openMedia(); !reopened) {
                app::warn(this, "Proxies", QString::fromStdString(reopened.error().toString()));
            }
            monitor_->update();
            refresh();
            break;
        }
        case chrome::ProxyChoice::Kind::Build: {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            auto made = buildProxy(chosen.media);
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
            break;
        }
        case chrome::ProxyChoice::Kind::Attach: {
            const QString path = QFileDialog::getOpenFileName(this, "Choose a proxy file");
            if (path.isEmpty()) {
                return;
            }
            for (model::MediaRef& media : document_.project().mediaMutable()) {
                if (media.id == chosen.media) {
                    media.proxyPath = path.toStdString();
                }
            }
            break;
        }
        case chrome::ProxyChoice::Kind::None:
            break;
    }
}

void PreviewWindow::multicamMenu() {
    const model::Clip* clip = editContext().selectedClip();
    const bool multicam = clip != nullptr && clip->isMulticam();
    switch (chrome::multicamMenu(multicam, media_ != nullptr)) {
        case chrome::MulticamChoice::ByAudio:
            syncAngles(true);
            break;
        case chrome::MulticamChoice::ByTimecode:
            syncAngles(false);
            break;
        case chrome::MulticamChoice::None:
            break;
    }
}

void PreviewWindow::renderMenu() {
    const model::Sequence* sequence = liveSequence();
    if (sequence == nullptr || media_ == nullptr) {
        return;
    }
    const time::TimeRange visible = timeline_->layout().visibleRange(sequence->frameRate());
    switch (chrome::renderMenu(visible.isEmpty() ? 0 : visible.duration().frames(),
                               renderCache_.count(), renderCache_.byteSize())) {
        case chrome::RenderChoice::ClearCache:
            renderCache_.clear();
            updateCacheBar();
            break;
        case chrome::RenderChoice::RenderVisible:
            renderVisibleRange();
            break;
        case chrome::RenderChoice::None:
            break;
    }
}

void PreviewWindow::loudnessMenu() {
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
    if (!chrome::loudnessMenu(measured->integratedLufs, measured->samplePeakDbfs, kTarget)) {
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
        if (auto built = edit::makeSetTrackState(document_.project(), liveSequence()->id(),
                                                 track.id(), state)) {
            document_.commands().execute(document_.project(), std::move(*built));
        }
    }
    document_.commands().breakMerge();
    mixer_->refresh();
}

void PreviewWindow::captionsMenu() {
    if (liveSequence() == nullptr) {
        return;
    }
    const model::CaptionTrack& captions = liveSequence()->captions();
    switch (chrome::captionsMenu(captions.size(), captions.isBurnedIn())) {
        case chrome::CaptionChoice::Import: {
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
            // The style and the burn-in setting belong to the sequence, not
            // to the file: importing a new script should not silently turn
            // burn-in off or lose a typeface somebody chose.
            model::CaptionTrack merged = *loaded;
            merged.setStyle(liveSequence()->captions().style());
            merged.setBurnedIn(liveSequence()->captions().isBurnedIn());
            applyCaptions(merged);
            break;
        }
        case chrome::CaptionChoice::Export: {
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
            break;
        }
        case chrome::CaptionChoice::ToggleBurnIn: {
            model::CaptionTrack changed = liveSequence()->captions();
            changed.setBurnedIn(!changed.isBurnedIn());
            applyCaptions(changed);
            break;
        }
        case chrome::CaptionChoice::None:
            break;
    }
}

void PreviewWindow::applyCaptions(const model::CaptionTrack& captions) {
    auto built = edit::makeSetCaptions(document_.project(), liveSequence()->id(), captions);
    if (!built) {
        return;
    }
    document_.commands().execute(document_.project(), std::move(*built));
    document_.commands().breakMerge();
    monitor_->update();
    timeline_->update();
    refreshInstruments();
}

}  // namespace zaro::app
