// The preview window.
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

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/core/edit/Sync.h"
#include "zaro/core/io/OtioIo.h"
#include "zaro/core/io/ProjectIo.h"
#include "zaro/core/io/SubtitleIo.h"
#include "zaro/core/playback/Transport.h"
#include "zaro/core/render/AudioGraph.h"
#include "zaro/core/render/RenderCache.h"
#include "zaro/core/render/RenderGraph.h"
#include "zaro/core/render/Scopes.h"
#include "zaro/core/time/Timecode.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"
#include "zaro/platform/qtext/QtTextRasterizer.h"
#include "zaro/platform/sdl/AudioSink.h"

#include "CurveEditor.h"
#include "EffectControls.h"
#include "ExportDialog.h"
#include "MixerPanel.h"
#include "ProgramMonitor.h"
#include "ProjectBin.h"
#include "ScopesPanel.h"
#include "SourceMonitor.h"
#include "TimelineWidget.h"

namespace {

using namespace zaro;

// No Q_OBJECT: this declares no signals or slots of its own, and
// QMetaObject::invokeMethod with a lambda needs only a QObject to bind the
// call to. Adding it in a .cpp would also require including its moc output.
class PreviewWindow : public QWidget {
public:
    PreviewWindow(model::Project project, io::LoadedProject loaded)
        : project_{std::move(project)}, loaded_{std::move(loaded)} {
        sequenceId_ = project_.activeSequence();

        monitor_ = new app::ProgramMonitor(this);
        monitor_->setMinimumSize(480, 270);

        timecode_ = new QLabel(this);
        // Ask the system for its fixed-width family rather than naming one:
        // a missing family costs a slow alias lookup and silently falls back.
        QFont monospace = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        monospace.setPointSize(15);
        timecode_->setFont(monospace);
        playButton_ = new QPushButton("Play", this);
        scrubber_ = new QSlider(Qt::Horizontal, this);

        auto* transportRow = new QHBoxLayout;
        transportRow->addWidget(playButton_);
        transportRow->addWidget(scrubber_, 1);
        transportRow->addWidget(timecode_);

        timeline_ = new app::TimelineWidget(this);
        effects_ = new app::EffectControls(this);
        scopes_ = new app::ScopesPanel(this);
        mixer_ = new app::MixerPanel(this);
        effects_->setMinimumWidth(250);
        effects_->setMaximumWidth(330);

        // Monitor and parameters side by side, transport under them, timeline
        // across the bottom.
        bin_ = new app::ProjectBin(this);
        bin_->setMinimumWidth(210);
        bin_->setMaximumWidth(280);

        source_ = new app::SourceMonitor(this);
        source_->setMinimumWidth(300);

        // Splitters rather than fixed layouts: panel sizes are a matter of what
        // someone is doing at the time, and the arrangement is remembered
        // between sessions.
        // The transport belongs under the program monitor, not at the bottom of
        // the window: it controls the picture above it, and putting the timeline
        // between them makes that relationship harder to see.
        // Export only, from the window. Importing an OTIO file produces a
        // project of its own, and replacing the open one needs a "save first?"
        // that does not exist yet -- so that direction lives in zaro-otio,
        // where there is nothing to lose.
        auto* otioButton = new QPushButton("Export OTIO…", this);
        otioButton->setToolTip("Write this sequence as an OpenTimelineIO file");
        otioButton->setMinimumWidth(otioButton->sizeHint().width());
        transportRow->addWidget(otioButton);
        connect(otioButton, &QPushButton::clicked, this, [this] {
            if (liveSequence() == nullptr) {
                return;
            }
            const QString path = QFileDialog::getSaveFileName(
                this, "Export OpenTimelineIO", "timeline.otio", "OpenTimelineIO (*.otio)");
            if (path.isEmpty()) {
                return;
            }
            if (Status saved = io::saveOtio(project_, liveSequence()->id(), path.toStdString());
                !saved) {
                QMessageBox::warning(this, "OpenTimelineIO",
                                     QString::fromStdString(saved.error().toString()));
            }
        });

        auto* proxyButton = new QPushButton("Proxies…", this);
        proxyButton->setToolTip("Attach smaller copies for editing");
        proxyButton->setMinimumWidth(proxyButton->sizeHint().width());
        transportRow->addWidget(proxyButton);
        connect(proxyButton, &QPushButton::clicked, this, [this] { proxyMenu(); });

        auto* loudnessButton = new QPushButton("Loudness…", this);
        loudnessButton->setToolTip("Measure the sequence and normalise to a target");
        loudnessButton->setMinimumWidth(loudnessButton->sizeHint().width());
        transportRow->addWidget(loudnessButton);
        connect(loudnessButton, &QPushButton::clicked, this, [this] { loudnessMenu(); });

        auto* multicamButton = new QPushButton("Multicam…", this);
        multicamButton->setToolTip("Work out the offsets between a clip's angles");
        multicamButton->setMinimumWidth(multicamButton->sizeHint().width());
        transportRow->addWidget(multicamButton);
        connect(multicamButton, &QPushButton::clicked, this, [this] { multicamMenu(); });

        auto* renderButton = new QPushButton("Render…", this);
        renderButton->setToolTip("Pre-render the visible range so it plays back");
        renderButton->setMinimumWidth(renderButton->sizeHint().width());
        transportRow->addWidget(renderButton);
        connect(renderButton, &QPushButton::clicked, this, [this] { renderMenu(); });

        auto* captionsButton = new QPushButton("Captions…", this);
        captionsButton->setToolTip("Import, export or burn in subtitles");
        captionsButton->setMinimumWidth(captionsButton->sizeHint().width());
        transportRow->addWidget(captionsButton);
        connect(captionsButton, &QPushButton::clicked, this, [this] { captionsMenu(); });

        auto* transportBar = new QWidget(this);
        transportBar->setLayout(transportRow);

        auto* programColumn = new QWidget(this);
        auto* programLayout = new QVBoxLayout(programColumn);
        programLayout->setContentsMargins(0, 0, 0, 0);
        programLayout->addWidget(monitor_, 1);
        programLayout->addWidget(transportBar);

        topSplitter_ = new QSplitter(Qt::Horizontal, this);
        topSplitter_->addWidget(bin_);
        topSplitter_->addWidget(source_);
        topSplitter_->addWidget(programColumn);
        // Scopes share the parameter column: they are read while grading, and
        // grading is done with the parameters in reach.
        auto* rightColumn = new QSplitter(Qt::Vertical, this);
        rightColumn->addWidget(effects_);
        rightColumn->addWidget(scopes_);
        rightColumn->addWidget(mixer_);
        rightColumn->setStretchFactor(0, 3);
        rightColumn->setStretchFactor(1, 1);
        rightColumn->setStretchFactor(2, 1);
        // Floors, so no panel can be squeezed to a sliver by its neighbours. A
        // scope four pixels tall or a mixer with no meter is worse than one
        // that pushes the window wider: it looks like a broken panel rather
        // than a small one.
        effects_->setMinimumHeight(220);
        scopes_->setMinimumHeight(150);
        mixer_->setMinimumHeight(190);
        topSplitter_->addWidget(rightColumn);
        topSplitter_->setStretchFactor(1, 1);
        topSplitter_->setStretchFactor(2, 2);

        connect(bin_, &app::ProjectBin::openRequested, this, [this](zaro::model::MediaRefId id) {
            if (const model::MediaRef* ref = project_.findMedia(id)) {
                source_->load(*ref);
            }
        });
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
        mainSplitter_->addWidget(topSplitter_);
        mainSplitter_->addWidget(timeline_);
        mainSplitter_->setStretchFactor(0, 3);
        mainSplitter_->setStretchFactor(1, 2);

        auto* layout = new QVBoxLayout(this);
        layout->addWidget(mainSplitter_, 1);

        connect(timeline_, &app::TimelineWidget::selectionChanged, effects_,
                &app::EffectControls::setSelection);
        // Kept here too: syncing acts on the clip somebody has picked, and the
        // timeline is where picking happens.
        connect(timeline_, &app::TimelineWidget::selectionChanged, this,
                [this](model::TrackId track, model::ClipId clip) {
                    selectedTrack_ = track;
                    selectedClip_ = clip;
                });
        connect(scopes_, &app::ScopesPanel::measurementNeeded, this, [this] { measureScopes(); });
        connect(mixer_, &app::MixerPanel::edited, this, [this] {
            // Mute and solo change the picture as well as the sound: a muted
            // video track stops being composited.
            monitor_->update();
            timeline_->update();
            measureScopes();
        });
        connect(effects_, &app::EffectControls::keyframesChanged, this,
                [this] { timeline_->update(); });
        connect(effects_, &app::EffectControls::edited, this, [this] {
            // A parameter change alters the picture at the current playhead.
            monitor_->update();
            timeline_->update();
            measureScopes();
        });

        // The two panels drive each other: scrubbing the timeline moves the
        // picture, and playback moves the playhead.
        connect(timeline_, &app::TimelineWidget::playheadMoved, this,
                [this](const time::RationalTime& position) {
                    stop();
                    setPosition(position);
                });
        connect(timeline_, &app::TimelineWidget::viewChanged, this, [this] { updateCacheBar(); });
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

        setWindowTitle("Zaro");
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
    [[nodiscard]] render::AudioSource& media() { return *media_; }
    [[nodiscard]] Status reopenMedia() { return openMedia(); }

    /// Re-seat everything that holds a pointer to the active sequence.
    ///
    /// Adding a sequence reallocates the project's vector of them, so anything
    /// holding a pointer into it is looking at freed memory. This window looks
    /// its own up by id; the monitor is handed one, so it has to be told again.
    void rebindSequence() {
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
            QMessageBox::warning(this, "Multicam",
                                 QString::fromStdString(synced.error().toString()));
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
                QMessageBox::warning(this, "Multicam",
                                     QString::fromStdString(built.error().toString()));
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
            QMessageBox::information(this, "Multicam",
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
            QMessageBox::warning(this, "Render", QString::fromStdString(stats.error().toString()));
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
        measureScopes();
        refresh();
    }

    void step(std::int64_t frames) {
        stop();
        setPosition(position_ + time::RationalTime{frames, liveSequence()->frameRate()});
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        // One table rather than a switch buried in a handler: the bindings are
        // the thing a user wants to see and eventually change, and a list of
        // them reads as documentation. Premiere's defaults, which is what hands
        // already know.
        struct Binding {
            int key;
            Qt::KeyboardModifiers modifiers;
            const char* description;
            void (PreviewWindow::*action)();
        };
        static const Binding kBindings[] = {
            {Qt::Key_I, Qt::NoModifier, "Mark in", &PreviewWindow::doMarkIn},
            {Qt::Key_O, Qt::NoModifier, "Mark out", &PreviewWindow::doMarkOut},
            {Qt::Key_Comma, Qt::NoModifier, "Insert from source", &PreviewWindow::doInsert},
            {Qt::Key_Period, Qt::NoModifier, "Overwrite from source", &PreviewWindow::doOverwrite},
            {Qt::Key_Left, Qt::ShiftModifier, "Previous marker", &PreviewWindow::doPreviousMarker},
            {Qt::Key_Right, Qt::ShiftModifier, "Next marker", &PreviewWindow::doNextMarker},
            {Qt::Key_Up, Qt::NoModifier, "Source back one frame", &PreviewWindow::doSourceBack},
            {Qt::Key_Down, Qt::NoModifier, "Source forward one frame",
             &PreviewWindow::doSourceForward},
        };
        for (const Binding& binding : kBindings) {
            if (event->key() == binding.key && event->modifiers() == binding.modifiers) {
                (this->*binding.action)();
                return;
            }
        }

        switch (event->key()) {
            case Qt::Key_Space:
                togglePlay();
                return;
            case Qt::Key_L:
                transport_.pressL();
                startIfPlaying();
                return;
            case Qt::Key_K:
                transport_.pressK();
                stop();
                return;
            case Qt::Key_J:
                transport_.pressJ();
                startIfPlaying();
                return;
            case Qt::Key_Left:
                step(-1);
                return;
            case Qt::Key_Right:
                step(1);
                return;
            case Qt::Key_Home:
                stop();
                setPosition(time::RationalTime{0, liveSequence()->frameRate()});
                return;
            case Qt::Key_End:
                stop();
                setPosition(liveSequence()->duration());
                return;
            case Qt::Key_E:
                if (event->modifiers().testFlag(Qt::ControlModifier) ||
                    event->modifiers().testFlag(Qt::MetaModifier)) {
                    stop();
                    app::ExportDialog dialog{project_, liveSequence()->id(), this};
                    dialog.exec();
                    return;
                }
                break;
            default:
                QWidget::keyPressEvent(event);
        }
    }

    void closeEvent(QCloseEvent* event) override {
        saveWorkspace();
        shutDown();
        QWidget::closeEvent(event);
    }

private:
    /// Panel sizes and window geometry, remembered between sessions.
    ///
    /// Saved on close rather than continuously: writing settings on every drag
    /// of a splitter is a lot of disk traffic for something only read once.
    void saveWorkspace() {
        QSettings settings("Zaro", "Zaro Video");
        settings.setValue("window/geometry", saveGeometry());
        settings.setValue("workspace/top", topSplitter_->saveState());
        settings.setValue("workspace/main", mainSplitter_->saveState());
    }

    void restoreWorkspace() {
        QSettings settings("Zaro", "Zaro Video");
        // Each restored only if it was stored, so a first run gets the
        // stretch factors set above rather than a collapsed layout.
        if (const auto geometry = settings.value("window/geometry").toByteArray();
            !geometry.isEmpty()) {
            restoreGeometry(geometry);
        }
        if (const auto state = settings.value("workspace/top").toByteArray(); !state.isEmpty()) {
            topSplitter_->restoreState(state);
        }
        if (const auto state = settings.value("workspace/main").toByteArray(); !state.isEmpty()) {
            mainSplitter_->restoreState(state);
        }
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
        playButton_->setText("Pause");

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
        playButton_->setText("Play");
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
    void measureScopes() {
        if (scopes_ == nullptr || media_ == nullptr || liveSequence() == nullptr) {
            return;
        }
        if (playing_ || !scopes_->wantsMeasurement()) {
            return;
        }
        render::RenderGraph graph{*media_};
        graph.setTextRasterizer(&text_);
        graph.setProject(&project_);
        auto frame = graph.composite(*liveSequence(), position_);
        if (!frame) {
            scopes_->clear();
            return;
        }
        render::ScopeOptions options;
        options.waveformColumns = std::max(64, scopes_->width());
        // Every second row. The shape of a waveform does not change for being
        // measured at half the vertical resolution, and this is running
        // between scrubs.
        options.rowStride = 2;
        scopes_->setScopes(render::measure(*frame, options));
    }

    /// Attach proxies, and switch between them and the originals.
    ///
    /// Attaching rather than generating: making a proxy is a transcode, and a
    /// transcode belongs to whatever tool the footage came out of. What this
    /// has to get right is the swap.
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
        for (const model::MediaRef& media : project_.media()) {
            QAction* action = menu.addAction(
                QString("Attach proxy for %1…").arg(QString::fromStdString(media.name)));
            attach.emplace(action, media.id);
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
                QMessageBox::warning(this, "Proxies",
                                     QString::fromStdString(reopened.error().toString()));
            }
            monitor_->update();
            refresh();
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
            QMessageBox::warning(this, "Loudness",
                                 QString::fromStdString(measured.error().toString()));
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
                QMessageBox::warning(this, "Subtitles",
                                     QString::fromStdString(loaded.error().toString()));
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
                QMessageBox::warning(this, "Subtitles",
                                     QString::fromStdString(saved.error().toString()));
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
        measureScopes();
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
    }

    model::Project project_;
    io::LoadedProject loaded_;
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
    /// What the timeline last said was picked. Multicam syncing acts on one
    /// clip, and this is which one.
    model::TrackId selectedTrack_;
    model::ClipId selectedClip_;
    std::int32_t lastSyncCount_{0};
    std::int32_t lastSyncSkipped_{0};

    app::ProgramMonitor* monitor_{nullptr};
    app::TimelineWidget* timeline_{nullptr};
    app::EffectControls* effects_{nullptr};
    app::ScopesPanel* scopes_{nullptr};
    app::MixerPanel* mixer_{nullptr};
    std::mutex meterMutex_;
    render::AudioGraph::Meters latestMeters_;
    QTimer* meterTimer_{nullptr};
    app::ProjectBin* bin_{nullptr};
    app::SourceMonitor* source_{nullptr};
    QSplitter* topSplitter_{nullptr};
    QSplitter* mainSplitter_{nullptr};
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

}  // namespace

namespace {

/// Drive a real drag through the widget, as the mouse would.
void dragOnTimeline(app::TimelineWidget* timeline, int fromX, int toX, int y,
                    Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const auto press = [&](QEvent::Type type, int x, Qt::MouseButton button,
                           Qt::MouseButtons buttons) {
        QMouseEvent event(type, QPointF(x, y), QPointF(x, y), button, buttons, modifiers);
        QCoreApplication::sendEvent(timeline, &event);
    };
    press(QEvent::MouseButtonPress, fromX, Qt::LeftButton, Qt::LeftButton);
    // In steps, because a trim is applied incrementally and a single jump would
    // not exercise the accumulation the real interaction relies on.
    const int steps = 8;
    for (int i = 1; i <= steps; ++i) {
        press(QEvent::MouseMove, fromX + (toX - fromX) * i / steps, Qt::NoButton, Qt::LeftButton);
    }
    press(QEvent::MouseButtonRelease, toX, Qt::LeftButton, Qt::NoButton);
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);

    QStringList arguments = QApplication::arguments();
    const bool selfTest = arguments.removeAll("--selftest") > 0;
    const bool editTest = arguments.removeAll("--selftest-edit") > 0;
    const bool quitTest = arguments.removeAll("--selftest-quit") > 0;
    QString capturePath;
    if (const auto at = arguments.indexOf("--capture"); at >= 0 && at + 1 < arguments.size()) {
        capturePath = arguments.at(at + 1);
        arguments.removeAt(at + 1);
        arguments.removeAt(at);
    }
    if (arguments.size() < 2) {
        std::puts("usage: zaro-preview <project.zaro> [--selftest]");
        std::puts("");
        std::puts("  space        play / pause        J K L   shuttle");
        std::puts("  left/right   step one frame      home/end  start / end");
        std::puts("");
        std::puts("  --selftest        render, verify a picture came out, exit");
        std::puts("  --capture <png>   with --selftest, save what the monitor showed");
        std::puts("  --selftest-edit   drive a trim and a drag through the timeline, exit");
        std::puts("  --selftest-quit   quit with background work in flight, exit");
        return 2;
    }

    zaro::platform::ffmpeg::installLogHandler(false);

    auto loaded = zaro::io::loadProject(arguments.at(1).toStdString());
    if (!loaded) {
        std::fprintf(stderr, "zaro-preview: %s\n", loaded.error().toString().c_str());
        return 1;
    }
    zaro::model::Project project = loaded->project;
    if (project.findSequence(project.activeSequence()) == nullptr) {
        std::fprintf(stderr, "zaro-preview: this project has no active sequence\n");
        return 1;
    }

    PreviewWindow window{std::move(project), std::move(*loaded)};
    if (const auto status = window.openMedia(); !status) {
        std::fprintf(stderr, "zaro-preview: %s\n", status.error().toString().c_str());
        return 1;
    }
    window.resize(960, 620);
    window.show();

    if (quitTest) {
        // Quit while the waveform thread is still running, which is what
        // happens when someone presses Cmd+Q on a freshly opened project.
        // The window is a local here, so returning destroys it -- and a
        // std::thread destroyed while still joinable calls std::terminate.
        // No joining, no waiting: that is the point.
        QApplication::processEvents();
        std::printf("zaro-preview quit selftest: exiting with background work in flight\n");
        return 0;
    }

    if (editTest) {
        // Exercise the timeline's editing interactions the way a mouse does.
        // The edit operations themselves are covered headlessly; what this
        // checks is the wiring -- that a drag reaches the right operation with
        // the right arguments, and that undo steps over the whole gesture.
        QApplication::processEvents();
        app::TimelineWidget* timeline = window.timeline();
        const zaro::model::Sequence& sequence = *window.sequence();

        const zaro::model::Track& videoTrack = sequence.videoTracks().front();
        if (videoTrack.clips().empty()) {
            std::fprintf(stderr, "zaro-preview: nothing on V1 to edit\n");
            return 1;
        }
        const zaro::model::Clip original = videoTrack.clips().front();
        const auto row = timeline->rowFor(videoTrack.id());
        if (!row) {
            std::fprintf(stderr, "zaro-preview: V1 has no row\n");
            return 1;
        }
        const int y = row->top + row->height / 2;

        std::printf("zaro-preview edit selftest\n");
        std::printf("  clip starts at %lld, %lld frames long\n",
                    static_cast<long long>(original.start().frames()),
                    static_cast<long long>(original.duration().frames()));

        // Trim the out point inwards by dragging its right edge left.
        const int outX = static_cast<int>(timeline->layout().xForTime(original.endExclusive()));
        const int wantedX = outX - 120;
        dragOnTimeline(timeline, outX - 2, wantedX, y);

        const zaro::model::Clip* trimmed =
            window.project().findSequence(sequence.id())->videoTracks().front().find(original.id);
        if (trimmed == nullptr) {
            std::fprintf(stderr, "  FAIL: the clip disappeared\n");
            return 1;
        }
        const std::int64_t shortened = original.duration().frames() - trimmed->duration().frames();
        std::printf("  after trimming out by 120px: %lld frames shorter\n",
                    static_cast<long long>(shortened));
        if (shortened <= 0) {
            std::fprintf(stderr, "  FAIL: the trim did not shorten the clip\n");
            return 1;
        }
        // The clip's start must not have moved: that is what distinguishes a
        // trim from a move.
        if (trimmed->start() != original.start()) {
            std::fprintf(stderr, "  FAIL: trimming the out point moved the clip\n");
            return 1;
        }

        // One undo, for the whole drag.
        const std::size_t depthBefore = window.commands().depth();
        window.commands().undo(window.project());
        const zaro::model::Clip* restored =
            window.project().findSequence(sequence.id())->videoTracks().front().find(original.id);
        if (restored == nullptr || restored->duration() != original.duration()) {
            std::fprintf(stderr, "  FAIL: one undo did not restore the clip\n");
            return 1;
        }
        std::printf("  one undo restored it (%zu command%s on the stack)\n", depthBefore,
                    depthBefore == 1 ? "" : "s");
        if (depthBefore != 1) {
            std::fprintf(stderr,
                         "  FAIL: the drag left %zu undo steps; it should coalesce to one\n",
                         depthBefore);
            return 1;
        }

        // A parameter change has to reach the picture, not just the model.
        // Rendering the same frame at full and at low opacity should differ;
        // if they do not, the compositor is not seeing what the panel wrote.
        window.setPosition(
            zaro::time::RationalTime{sequence.duration().frames() / 2, sequence.frameRate()});
        QApplication::processEvents();
        const QImage before = window.monitor()->grabFramebuffer();

        zaro::model::Transform faded;
        faded.opacity = 0.15;
        auto dim = zaro::edit::makeSetTransform(window.project(), {sequence.id(), videoTrack.id()},
                                                original.id, faded);
        if (!dim) {
            std::fprintf(stderr, "  FAIL: %s\n", dim.error().toString().c_str());
            return 1;
        }
        window.commands().execute(window.project(), std::move(*dim));
        window.monitor()->update();
        QApplication::processEvents();
        const QImage after = window.monitor()->grabFramebuffer();

        const auto meanGray = [](const QImage& image) {
            if (image.isNull()) {
                return 0.0;
            }
            double total = 0.0;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    total += qGray(image.pixel(x, y));
                }
            }
            return total / (image.width() * image.height());
        };
        const double brightBefore = meanGray(before);
        const double brightAfter = meanGray(after);
        std::printf("  opacity 1.0 -> 0.15 changed mean brightness %.1f -> %.1f\n", brightBefore,
                    brightAfter);
        if (!(brightAfter < brightBefore * 0.6)) {
            std::fprintf(stderr,
                         "  FAIL: lowering opacity did not darken the picture; the panel and "
                         "the compositor are not connected\n");
            return 1;
        }

        // Keyframes have to reach the GPU compositor, not just the CPU one.
        // The two traversals are separate code, so a curve honoured on export
        // and ignored in preview is a bug nothing else here would catch: the
        // headless render tests only exercise render::RenderGraph.
        {
            // Undo the dim above before taking any pointer into the model: undo
            // restores a snapshot, which replaces the clips wholesale.
            window.commands().undo(window.project());
            QApplication::processEvents();

            const auto brightnessAt = [&](std::int64_t frame) {
                window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
                window.monitor()->update();
                QApplication::processEvents();
                return meanGray(window.monitor()->grabFramebuffer());
            };

            // This fixture is black except on its flash frames, so a fade has to
            // be measured on a frame that is lit to begin with. Measuring an
            // arbitrary frame would report a working fade on footage that was
            // already black.
            // Not from frame zero: the fade is anchored at the clip's first
            // frame, so a lit frame there would give the ramp no length at all
            // and both keyframes would land on the same instant.
            std::int64_t litFrame = -1;
            double baseline = 0.0;
            for (std::int64_t frame = 8; frame < 60; ++frame) {
                const double gray = brightnessAt(frame);
                if (gray > baseline) {
                    baseline = gray;
                    litFrame = frame;
                }
            }
            if (litFrame < 0 || baseline < 40.0) {
                std::fprintf(stderr, "  FAIL: no lit frame to fade\n");
                return 1;
            }

            zaro::model::Clip* clip = window.project()
                                          .findSequence(sequence.id())
                                          ->tracksMutable(zaro::model::TrackKind::Video)
                                          .front()
                                          .find(original.id);
            if (clip == nullptr) {
                std::fprintf(stderr, "  FAIL: the clip vanished\n");
                return 1;
            }

            // Keyframes are in source time. Anchoring the fade so that the lit
            // frame lands at a known point on the curve is what makes this a
            // measurement of the interpolation rather than of the footage.
            const zaro::time::RationalTime litSource =
                clip->sourceTimeAt(zaro::time::RationalTime{litFrame, sequence.frameRate()});
            const auto setFade = [&](std::int64_t spanFrames) {
                zaro::model::Keyframe lit;
                lit.time = clip->sourceRange.start();
                lit.value = 1.0;
                zaro::model::Keyframe dark;
                dark.time = clip->sourceRange.start() +
                            zaro::time::RationalTime{spanFrames, litSource.rate()};
                dark.value = 0.0;
                clip->animation.erase(zaro::model::Param::Opacity);
                clip->animation.curve(zaro::model::Param::Opacity).set(lit);
                clip->animation.curve(zaro::model::Param::Opacity).set(dark);
            };

            const std::int64_t intoClip = litSource.frames() - clip->sourceRange.start().frames();
            setFade(intoClip * 2);  // the lit frame sits halfway down the ramp
            const double halfway = brightnessAt(litFrame);
            setFade(intoClip);  // and now exactly at its end
            const double gone = brightnessAt(litFrame);
            clip->animation.erase(zaro::model::Param::Opacity);

            std::printf("  keyframed fade on the GPU: %.1f lit, %.1f halfway, %.1f faded\n",
                        baseline, halfway, gone);
            if (!(halfway > baseline * 0.3) || !(halfway < baseline * 0.75)) {
                std::fprintf(stderr, "  FAIL: the GPU compositor is not interpolating keyframes\n");
                return 1;
            }
            if (!(gone < baseline * 0.1)) {
                std::fprintf(stderr,
                             "  FAIL: the GPU compositor is not reading keyframes; preview and "
                             "export would disagree\n");
                return 1;
            }
        }

        // The scopes, end to end: a measurement has to reach the panel and be
        // drawn there, and it has to be drawn the right way up. Where the trace
        // sits is the assertion, not how many pixels it covers -- the
        // measurement is in signal order, where 0 is black, and the screen is
        // upside down relative to it. Getting that backwards produces a scope
        // that looks entirely plausible and reports the opposite of the truth.
        {
            // Returns the mean row of the trace, as a fraction of the plot
            // area: 0 is the top of the scope and 1 the bottom.
            const auto traceHeight = [&]() -> double {
                const QImage shot = window.scopes()->grab().toImage();
                const auto dpr = static_cast<int>(shot.devicePixelRatio());
                const QRect plot = window.scopes()->plotArea();
                const int top = plot.top() * dpr;
                const int bottom = std::min((plot.bottom() + 1) * dpr, shot.height());
                double weighted = 0.0;
                std::int64_t lit = 0;
                for (int y = top; y < bottom; ++y) {
                    for (int x = plot.left() * dpr;
                         x < std::min((plot.right() + 1) * dpr, shot.width()); ++x) {
                        if (qGray(shot.pixel(x, y)) > 110) {
                            weighted += y - top;
                            ++lit;
                        }
                    }
                }
                return lit == 0 ? -1.0
                                : weighted / static_cast<double>(lit) / std::max(1, bottom - top);
            };

            // This fixture is black except on its flash frames, so it offers a
            // bright frame and a dark one without any setup.
            double brightest = 0.0;
            double darkest = 0.0;
            double atBrightest = -1.0;
            double atDarkest = -1.0;
            for (std::int64_t frame = 0; frame < 40; ++frame) {
                window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
                QApplication::processEvents();
                const double picture = meanGray(window.monitor()->grabFramebuffer());
                const double where = traceHeight();
                if (where < 0.0) {
                    std::fprintf(stderr, "  FAIL: the scope drew no trace at all\n");
                    return 1;
                }
                if (atBrightest < 0.0 || picture > brightest) {
                    brightest = picture;
                    atBrightest = where;
                }
                if (atDarkest < 0.0 || picture < darkest) {
                    darkest = picture;
                    atDarkest = where;
                }
            }
            std::printf(
                "  scope trace sits at %.2f of the plot on the brightest frame, %.2f on "
                "the darkest\n",
                atBrightest, atDarkest);
            if (!(atBrightest < atDarkest)) {
                std::fprintf(stderr,
                             "  FAIL: the scope puts a bright picture no higher than a dark one, "
                             "so it is drawn upside down or not measuring the picture\n");
                return 1;
            }
            if (atDarkest < 0.8) {
                std::fprintf(stderr,
                             "  FAIL: a black frame should read at the bottom of the scope\n");
                return 1;
            }
        }

        // Colour correction, through the panel and out to the picture. The
        // grade is separate code on the CPU and the GPU, and the unit tests
        // compare those two directly -- what they cannot see is whether the
        // panel is wired to either of them.
        {
            auto* exposure = window.effects()
                                 ->findChild<QToolButton*>("keyframe:exposure")
                                 ->parentWidget()
                                 ->findChild<QDoubleSpinBox*>();
            auto* saturation = window.effects()
                                   ->findChild<QToolButton*>("keyframe:saturation")
                                   ->parentWidget()
                                   ->findChild<QDoubleSpinBox*>();
            if (exposure == nullptr || saturation == nullptr) {
                std::fprintf(stderr, "  FAIL: the colour controls are missing\n");
                return 1;
            }
            window.effects()->setSelection(videoTrack.id(), original.id);

            // A frame that is lit to begin with: exposure on black is black.
            std::int64_t litFrame = 0;
            double litness = 0.0;
            for (std::int64_t frame = 0; frame < 40; ++frame) {
                window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
                QApplication::processEvents();
                const double gray = meanGray(window.monitor()->grabFramebuffer());
                if (gray > litness) {
                    litness = gray;
                    litFrame = frame;
                }
            }
            window.setPosition(zaro::time::RationalTime{litFrame, sequence.frameRate()});
            QApplication::processEvents();

            const double litBefore = meanGray(window.monitor()->grabFramebuffer());
            exposure->setValue(-2.0);
            QApplication::processEvents();
            window.monitor()->update();
            QApplication::processEvents();
            const double darker = meanGray(window.monitor()->grabFramebuffer());

            exposure->setValue(0.0);
            saturation->setValue(0.0);
            QApplication::processEvents();
            window.monitor()->update();
            QApplication::processEvents();
            const QImage grey = window.monitor()->grabFramebuffer();
            std::int64_t coloured = 0;
            std::int64_t looked = 0;
            for (int gy = 0; gy < grey.height(); gy += 3) {
                for (int gx = 0; gx < grey.width(); gx += 3) {
                    const QColor sample = grey.pixelColor(gx, gy);
                    ++looked;
                    if (std::abs(sample.red() - sample.green()) > 4 ||
                        std::abs(sample.green() - sample.blue()) > 4) {
                        ++coloured;
                    }
                }
            }
            std::printf(
                "  colour: two stops down %.1f -> %.1f, monochrome leaves %lld of %lld "
                "pixels coloured\n",
                litBefore, darker, static_cast<long long>(coloured),
                static_cast<long long>(looked));

            if (!(darker < litBefore * 0.6)) {
                std::fprintf(stderr,
                             "  FAIL: two stops of exposure did not darken the picture; the "
                             "panel and the compositor are not connected\n");
                return 1;
            }
            if (coloured > looked / 100) {
                std::fprintf(stderr, "  FAIL: zero saturation left colour in the picture\n");
                return 1;
            }

            saturation->setValue(100.0);
            QApplication::processEvents();
            while (window.commands().canUndo()) {
                window.commands().undo(window.project());
            }
            window.monitor()->update();
            QApplication::processEvents();
        }

        // A generated shape, created and then edited through the panel, and
        // measured through the real GPU compositor. A shape is drawn on the CPU
        // and uploaded, so this also checks that path is reachable at all.
        {
            const auto videoTrackId = videoTrack.id();

            // The darkest frame is chosen *before* the shape is added: with a
            // white rectangle covering the frame every position reads the same,
            // and the search would settle on whichever came first -- which on
            // this fixture is a flash frame that is white anyway.
            std::int64_t darkFrame = 0;
            double darkest = 1e9;
            for (std::int64_t frame = 0; frame < 35; ++frame) {
                window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
                QApplication::processEvents();
                const double gray = meanGray(window.monitor()->grabFramebuffer());
                if (gray < darkest) {
                    darkest = gray;
                    darkFrame = frame;
                }
            }
            window.setPosition(zaro::time::RationalTime{darkFrame, sequence.frameRate()});
            QApplication::processEvents();
            const double without = meanGray(window.monitor()->grabFramebuffer());

            zaro::model::Graphic shape;
            shape.kind = zaro::model::GraphicKind::Rectangle;
            shape.width = 4000.0;  // larger than the frame, so it fills it
            shape.height = 4000.0;
            shape.red = 1.0;
            shape.green = 1.0;
            shape.blue = 1.0;

            const auto& videoTracks = window.project().findSequence(sequence.id())->videoTracks();
            const auto topTrack = videoTracks.size() > 1 ? videoTracks[1].id() : videoTrackId;
            auto built = zaro::edit::makeAddGraphic(
                window.project(), {sequence.id(), topTrack}, shape,
                zaro::time::TimeRange{zaro::time::RationalTime{0, sequence.frameRate()},
                                      zaro::time::RationalTime{40, sequence.frameRate()}});
            if (!built) {
                std::fprintf(stderr, "  FAIL: %s\n", built.error().toString().c_str());
                return 1;
            }
            window.commands().execute(window.project(), std::move(*built));
            window.monitor()->update();
            QApplication::processEvents();
            const double withShape = meanGray(window.monitor()->grabFramebuffer());

            std::printf("  shape layer: %.1f with a white rectangle, %.1f without\n", withShape,
                        without);
            if (!(withShape > without + 50.0)) {
                std::fprintf(stderr, "  FAIL: the shape layer did not reach the preview\n");
                return 1;
            }

            while (window.commands().canUndo()) {
                window.commands().undo(window.project());
            }
            window.monitor()->update();
            QApplication::processEvents();
        }

        // Proxies: the swap, and the promise that export ignores it. The proxy
        // fixture is inverted as well as smaller, so which file was read is
        // unmistakable rather than a matter of judging sharpness.
        {
            for (auto& media : window.project().mediaMutable()) {
                media.proxyPath =
                    "/private/tmp/claude-1970005770/-Users-anthony-lazzaro-Documents-Zaro-Video/"
                    "5921f2b6-fdbc-4e60-9098-ed4fd2d5a97a/scratchpad/proxy.mov";
            }

            std::int64_t brightFrame = 0;
            double brightness = 0.0;
            for (std::int64_t frame = 0; frame < 40; ++frame) {
                window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
                QApplication::processEvents();
                const double gray = meanGray(window.monitor()->grabFramebuffer());
                if (gray > brightness) {
                    brightness = gray;
                    brightFrame = frame;
                }
            }
            window.setPosition(zaro::time::RationalTime{brightFrame, sequence.frameRate()});
            QApplication::processEvents();
            const double fromOriginal = meanGray(window.monitor()->grabFramebuffer());

            window.project().setUsingProxies(true);
            if (Status reopened = window.reopenMedia(); !reopened) {
                std::fprintf(stderr, "  FAIL: %s\n", reopened.error().toString().c_str());
                return 1;
            }
            window.monitor()->update();
            QApplication::processEvents();
            const double viaProxy = meanGray(window.monitor()->grabFramebuffer());

            std::printf("  proxies: %.1f from the original, %.1f from an inverted proxy\n",
                        fromOriginal, viaProxy);
            if (!(std::fabs(fromOriginal - viaProxy) > 40.0)) {
                std::fprintf(stderr, "  FAIL: switching to proxies did not change what was read\n");
                return 1;
            }

            // Back to the originals, which is also what export uses whatever
            // this toggle says.
            window.project().setUsingProxies(false);
            if (Status reopened = window.reopenMedia(); !reopened) {
                std::fprintf(stderr, "  FAIL: %s\n", reopened.error().toString().c_str());
                return 1;
            }
            for (auto& media : window.project().mediaMutable()) {
                media.proxyPath.clear();
            }
            window.monitor()->update();
            QApplication::processEvents();
        }

        // Loudness, measured through the real mix and then normalised. The
        // meter is tested headlessly against the standard's calibration case;
        // what that cannot show is whether measuring a sequence and acting on
        // the answer actually moves the programme.
        {
            zaro::render::AudioGraph loudnessMix{window.media()};
            const zaro::time::TimeRange whole{zaro::time::RationalTime{0, sequence.frameRate()},
                                              window.sequence()->duration()};
            auto wasAt = loudnessMix.measureLoudness(*window.sequence(), whole);
            if (!wasAt) {
                std::fprintf(stderr, "  FAIL: %s\n", wasAt.error().toString().c_str());
                return 1;
            }
            if (!(wasAt->integratedLufs > zaro::render::LoudnessMeter::kSilence)) {
                std::fprintf(stderr, "  FAIL: the sequence measured as silence\n");
                return 1;
            }

            constexpr double kTarget = -23.0;
            const double gain = wasAt->gainToReach(kTarget);
            const auto audioTrack =
                window.project().findSequence(sequence.id())->audioTracks().front();
            zaro::edit::TrackState state;
            state.gainDb = audioTrack.gainDb() + gain;
            auto built = zaro::edit::makeSetTrackState(window.project(), sequence.id(),
                                                       audioTrack.id(), state);
            if (!built) {
                std::fprintf(stderr, "  FAIL: %s\n", built.error().toString().c_str());
                return 1;
            }
            window.commands().execute(window.project(), std::move(*built));

            zaro::render::AudioGraph afterMix{window.media()};
            auto now = afterMix.measureLoudness(*window.sequence(), whole);
            if (!now) {
                std::fprintf(stderr, "  FAIL: %s\n", now.error().toString().c_str());
                return 1;
            }
            std::printf("  loudness: %.1f LUFS, %+.1f dB applied, now %.1f LUFS\n",
                        wasAt->integratedLufs, gain, now->integratedLufs);
            if (std::fabs(now->integratedLufs - kTarget) > 0.5) {
                std::fprintf(stderr, "  FAIL: normalising did not land on the target\n");
                return 1;
            }

            while (window.commands().canUndo()) {
                window.commands().undo(window.project());
            }
            window.mixer()->refresh();
        }

        // The processing chain, through the mixer. The filters and the
        // compressor are tested headlessly against known responses; what that
        // cannot show is whether the strip's buttons reach the mix.
        {
            const auto audioTrackId =
                window.project().findSequence(sequence.id())->audioTracks().front().id();
            auto* eqBox = window.mixer()->findChild<QCheckBox*>();
            if (eqBox == nullptr) {
                std::fprintf(stderr, "  FAIL: the mixer strip has no controls\n");
                return 1;
            }

            // A hard low pass: the fixture's audio is clicks, which are almost
            // entirely high frequency, so removing the top takes most of it.
            zaro::model::AudioEq eq;
            eq.enabled = true;
            eq.lowPassHz = 300.0;
            auto built = zaro::edit::makeSetTrackProcessing(
                window.project(), sequence.id(), audioTrackId, eq, zaro::model::Compressor{});
            if (!built) {
                std::fprintf(stderr, "  FAIL: %s\n", built.error().toString().c_str());
                return 1;
            }

            const auto loudestNow = [&]() {
                zaro::render::AudioGraph probe{window.media()};
                float peak = 0.0F;
                const auto& audioRate = sequence.audioSampleRate();
                for (std::int64_t frame = 0; frame < 60; ++frame) {
                    probe.resetProcessing();
                    const zaro::time::RationalTime at{frame, sequence.frameRate()};
                    if (auto mixed =
                            probe.mix(*window.sequence(), at.rescaledTo(audioRate), 2048, 2)) {
                        for (std::int64_t i = 0; i < mixed->sampleCount(); ++i) {
                            peak = std::max(peak, std::fabs(mixed->channel(0)[i]));
                        }
                    }
                }
                return peak;
            };

            const float plain = loudestNow();
            window.commands().execute(window.project(), std::move(*built));
            const float filtered = loudestNow();

            std::printf("  audio processing: %.3f plain, %.3f through a 300 Hz low pass\n",
                        static_cast<double>(plain), static_cast<double>(filtered));
            if (!(plain > 0.05F)) {
                std::fprintf(stderr, "  FAIL: nothing to filter\n");
                return 1;
            }
            if (!(filtered < plain * 0.5F)) {
                std::fprintf(stderr, "  FAIL: the low pass did not reach the mix\n");
                return 1;
            }

            while (window.commands().canUndo()) {
                window.commands().undo(window.project());
            }
            window.mixer()->refresh();
        }

        // A mask, through the panel and the real GPU compositor. The geometry
        // is compared against the CPU headlessly; what that cannot show is
        // whether these controls reach the picture.
        {
            auto* shapeBox = window.effects()->findChild<QComboBox*>("mask-shape");
            auto* inverted = window.effects()->findChild<QCheckBox*>("mask-inverted");
            if (shapeBox == nullptr || inverted == nullptr) {
                std::fprintf(stderr, "  FAIL: the mask controls are missing\n");
                return 1;
            }
            window.effects()->setSelection(videoTrack.id(), original.id);
            QApplication::processEvents();

            // A lit frame, so a mask that hides most of it is unmistakable.
            std::int64_t litFrame = 0;
            double litness = 0.0;
            for (std::int64_t frame = 0; frame < 40; ++frame) {
                window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
                QApplication::processEvents();
                const double gray = meanGray(window.monitor()->grabFramebuffer());
                if (gray > litness) {
                    litness = gray;
                    litFrame = frame;
                }
            }
            window.setPosition(zaro::time::RationalTime{litFrame, sequence.frameRate()});
            QApplication::processEvents();
            const double whole = meanGray(window.monitor()->grabFramebuffer());

            // A small ellipse: most of the picture goes.
            zaro::model::Mask mask;
            mask.shape = zaro::model::MaskShape::Ellipse;
            mask.width = 60.0;
            mask.height = 60.0;
            auto built = zaro::edit::makeSetMask(window.project(), {sequence.id(), videoTrack.id()},
                                                 original.id, mask);
            if (!built) {
                std::fprintf(stderr, "  FAIL: %s\n", built.error().toString().c_str());
                return 1;
            }
            window.commands().execute(window.project(), std::move(*built));
            window.monitor()->update();
            QApplication::processEvents();
            const double throughMask = meanGray(window.monitor()->grabFramebuffer());

            // Inverted, the same mask keeps everything it was hiding.
            mask.inverted = true;
            auto flipped = zaro::edit::makeSetMask(
                window.project(), {sequence.id(), videoTrack.id()}, original.id, mask);
            window.commands().execute(window.project(), std::move(*flipped));
            window.monitor()->update();
            QApplication::processEvents();
            const double outside = meanGray(window.monitor()->grabFramebuffer());

            std::printf("  mask: %.1f whole, %.1f through a small ellipse, %.1f inverted\n", whole,
                        throughMask, outside);
            if (!(throughMask < whole * 0.5)) {
                std::fprintf(stderr, "  FAIL: the mask did not hide anything\n");
                return 1;
            }
            if (!(outside > throughMask * 2.0)) {
                std::fprintf(stderr, "  FAIL: inverting the mask did not swap what it keeps\n");
                return 1;
            }
            // The two halves have to add up to the whole, since one keeps
            // exactly what the other discards.
            if (std::fabs((throughMask + outside) - whole) > whole * 0.05) {
                std::fprintf(stderr,
                             "  FAIL: a mask and its inverse do not add up to the picture "
                             "(%.1f + %.1f against %.1f)\n",
                             throughMask, outside, whole);
                return 1;
            }

            while (window.commands().canUndo()) {
                window.commands().undo(window.project());
            }
            window.effects()->refresh();
            window.monitor()->update();
            QApplication::processEvents();
        }

        // Captions: imported from a real file, burned in, and measured through
        // the compositor. The parser and the burn-in are tested headlessly;
        // what those cannot show is whether an imported file reaches the
        // picture.
        {
            auto subtitles = zaro::io::loadSubtitles(
                "/private/tmp/claude-1970005770/-Users-anthony-lazzaro-Documents-Zaro-Video/"
                "5921f2b6-fdbc-4e60-9098-ed4fd2d5a97a/scratchpad/test.srt");
            if (!subtitles) {
                std::fprintf(stderr, "  FAIL: %s\n", subtitles.error().toString().c_str());
                return 1;
            }
            if (subtitles->size() != 2) {
                std::fprintf(stderr, "  FAIL: read %zu captions, expected 2\n", subtitles->size());
                return 1;
            }

            std::int64_t darkFrame = 0;
            double darkest = 1e9;
            for (std::int64_t frame = 0; frame < 40; ++frame) {
                window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
                QApplication::processEvents();
                const double gray = meanGray(window.monitor()->grabFramebuffer());
                if (gray < darkest) {
                    darkest = gray;
                    darkFrame = frame;
                }
            }

            zaro::model::CaptionTrack burned = *subtitles;
            burned.setBurnedIn(true);
            auto built = zaro::edit::makeSetCaptions(window.project(), sequence.id(), burned);
            if (!built) {
                std::fprintf(stderr, "  FAIL: %s\n", built.error().toString().c_str());
                return 1;
            }
            window.commands().execute(window.project(), std::move(*built));

            // The darkest frame inside the caption's span, not frame zero: this
            // fixture's frame zero is a white flash, and a white caption on a
            // white frame is invisible. The caption runs to two seconds, which
            // covers the whole range searched above.
            window.setPosition(zaro::time::RationalTime{darkFrame, sequence.frameRate()});
            window.monitor()->update();
            QApplication::processEvents();
            const double withCaption = meanGray(window.monitor()->grabFramebuffer());

            window.commands().undo(window.project());
            window.monitor()->update();
            QApplication::processEvents();
            const double without = meanGray(window.monitor()->grabFramebuffer());

            std::printf("  captions: %zu read, frame reads %.2f burned in against %.2f\n",
                        subtitles->size(), withCaption, without);
            if (!(withCaption > without + 0.2)) {
                std::fprintf(stderr, "  FAIL: the burned-in caption did not reach the picture\n");
                return 1;
            }

            while (window.commands().canUndo()) {
                window.commands().undo(window.project());
            }
            window.monitor()->update();
            QApplication::processEvents();
        }

        // A text layer, through the real font engine and the real compositor.
        // The coverage-to-colour step is tested headlessly against a stand-in
        // engine; what that cannot show is whether Qt is actually being asked
        // for glyphs and whether they reach the screen.
        {
            std::int64_t darkFrame = 0;
            double darkest = 1e9;
            for (std::int64_t frame = 0; frame < 35; ++frame) {
                window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
                QApplication::processEvents();
                const double gray = meanGray(window.monitor()->grabFramebuffer());
                if (gray < darkest) {
                    darkest = gray;
                    darkFrame = frame;
                }
            }
            window.setPosition(zaro::time::RationalTime{darkFrame, sequence.frameRate()});
            QApplication::processEvents();
            const double blank = meanGray(window.monitor()->grabFramebuffer());

            zaro::model::Graphic title;
            title.kind = zaro::model::GraphicKind::Text;
            title.text = "ZARO";
            title.pointSize = 160.0;
            title.bold = true;
            title.width = 2000.0;
            title.height = 800.0;
            title.red = 1.0;
            title.green = 1.0;
            title.blue = 1.0;

            auto built = zaro::edit::makeAddGraphic(
                window.project(), {sequence.id(), videoTrack.id()}, title,
                zaro::time::TimeRange{zaro::time::RationalTime{0, sequence.frameRate()},
                                      zaro::time::RationalTime{40, sequence.frameRate()}});
            if (!built) {
                std::fprintf(stderr, "  FAIL: %s\n", built.error().toString().c_str());
                return 1;
            }
            window.commands().execute(window.project(), std::move(*built));
            window.monitor()->update();
            QApplication::processEvents();
            const double withText = meanGray(window.monitor()->grabFramebuffer());

            // Glyphs cover a small fraction of the frame, so this is not a big
            // number -- but it is unambiguously more than nothing, and nothing
            // is what a missing font engine produces.
            std::printf("  text layer: %.2f with a title, %.2f without\n", withText, blank);
            if (!(withText > blank + 0.5)) {
                std::fprintf(stderr,
                             "  FAIL: the text layer drew nothing; either Qt was not asked for "
                             "glyphs or they did not reach the compositor\n");
                return 1;
            }

            while (window.commands().canUndo()) {
                window.commands().undo(window.project());
            }
            window.monitor()->update();
            QApplication::processEvents();
        }

        // The mixer: solo, and the meters. Solo is a rule about the whole
        // sequence rather than a property of one track, so the check is that
        // soloing one track silences another.
        {
            auto* audioTrack = window.project()
                                   .findSequence(sequence.id())
                                   ->tracksMutable(zaro::model::TrackKind::Audio)
                                   .data();
            if (audioTrack == nullptr) {
                std::fprintf(stderr, "  FAIL: no audio track to mix\n");
                return 1;
            }
            window.mixer()->refresh();
            QApplication::processEvents();

            // The meters are only updated while the panel is visible, which is
            // the right behaviour and a trap for a test: whether a widget has
            // become visible depends on how many event loops have run, so this
            // asks explicitly rather than depending on it. Left implicit it
            // read zero once in five runs and looked like a broken mixer.
            for (int i = 0; i < 5 && !window.mixer()->isVisible(); ++i) {
                QApplication::processEvents();
            }
            if (!window.mixer()->isVisible()) {
                std::fprintf(stderr, "  FAIL: the mixer panel never became visible\n");
                return 1;
            }

            auto* meter = window.mixer()->findChild<app::LevelMeter*>(
                QString{"mixer-meter-"} + QString::number(audioTrack->id().value()));
            auto* master = window.mixer()->findChild<app::LevelMeter*>("mixer-master-meter");
            if (meter == nullptr || master == nullptr) {
                std::fprintf(stderr, "  FAIL: the mixer has no meters\n");
                return 1;
            }

            // This fixture's audio is clicks a second apart, so most positions
            // are silence -- and a peak hold is designed to keep showing the
            // last loud thing, so reading it at one arbitrary position gives
            // either the click or whatever was held from somewhere else. Scan
            // instead, and take the loudest.
            const auto loudest = [&](app::LevelMeter* which) {
                float peak = 0.0F;
                for (std::int64_t frame = 0; frame < 60; ++frame) {
                    which->resetHold();
                    window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
                    QApplication::processEvents();
                    window.updateMeters();
                    peak = std::max(peak, which->hold());
                }
                return peak;
            };
            const float heard = loudest(meter);
            const float masterHeard = loudest(master);

            // Solo a *video* track: the audio track is not soloed, so it falls
            // silent even though nobody muted it.
            auto* videoSolo = window.project()
                                  .findSequence(sequence.id())
                                  ->tracksMutable(zaro::model::TrackKind::Video)
                                  .data();
            zaro::edit::TrackState soloed;
            soloed.soloed = true;
            auto built = zaro::edit::makeSetTrackState(window.project(), sequence.id(),
                                                       videoSolo->id(), soloed);
            if (!built) {
                std::fprintf(stderr, "  FAIL: %s\n", built.error().toString().c_str());
                return 1;
            }
            window.commands().execute(window.project(), std::move(*built));
            window.mixer()->refresh();
            const float silenced = loudest(meter);

            std::printf(
                "  mixer meters: %.3f heard, %.3f once something else is soloed "
                "(master %.3f)\n",
                static_cast<double>(heard), static_cast<double>(silenced),
                static_cast<double>(masterHeard));
            if (!(heard > 0.01F)) {
                std::fprintf(stderr, "  FAIL: the meters read nothing on a clip with sound\n");
                return 1;
            }
            if (!(masterHeard > 0.01F)) {
                std::fprintf(stderr, "  FAIL: the master meter reads nothing\n");
                return 1;
            }
            if (!(silenced < heard * 0.05F)) {
                std::fprintf(stderr, "  FAIL: soloing another track did not silence this one\n");
                return 1;
            }

            while (window.commands().canUndo()) {
                window.commands().undo(window.project());
            }
            window.mixer()->refresh();
        }

        // A look LUT, loaded from a real file through the model the panel
        // writes to. The parser and the baked cube are tested headlessly and
        // the two render paths are compared; what is left is whether a LUT set
        // on a clip reaches the picture at all.
        {
            const auto clipNow = [&]() {
                return window.project()
                    .findSequence(sequence.id())
                    ->videoTracks()
                    .front()
                    .find(original.id);
            };
            // A dark frame: this look lifts black by 0.15, which a black frame
            // shows and a white one cannot.
            // Both extremes of the fixture. The bright one is the reference the
            // lift is judged against: an absolute threshold would be measuring
            // how much of the monitor the letterbox covers, which moves
            // whenever a panel is added -- it has already been wrong twice.
            std::int64_t darkFrame = 0;
            double darkness = 1e9;
            double brightest = 0.0;
            for (std::int64_t frame = 0; frame < 40; ++frame) {
                window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
                QApplication::processEvents();
                const double gray = meanGray(window.monitor()->grabFramebuffer());
                if (gray < darkness) {
                    darkness = gray;
                    darkFrame = frame;
                }
                brightest = std::max(brightest, gray);
            }
            window.setPosition(zaro::time::RationalTime{darkFrame, sequence.frameRate()});
            QApplication::processEvents();
            const double plainDark = meanGray(window.monitor()->grabFramebuffer());

            zaro::model::LutRef look;
            look.path =
                "/private/tmp/claude-1970005770/-Users-anthony-lazzaro-Documents-Zaro-Video/"
                "5921f2b6-fdbc-4e60-9098-ed4fd2d5a97a/scratchpad/warm.cube";
            auto built = zaro::edit::makeSetLut(window.project(), {sequence.id(), videoTrack.id()},
                                                original.id, look);
            if (!built) {
                std::fprintf(stderr, "  FAIL: %s\n", built.error().toString().c_str());
                return 1;
            }
            window.commands().execute(window.project(), std::move(*built));
            window.monitor()->update();
            QApplication::processEvents();
            const double lifted = meanGray(window.monitor()->grabFramebuffer());

            // And dialled back to nothing, which has to return the picture.
            zaro::model::LutRef none = clipNow()->lut;
            none.amount = 0.0;
            auto cleared = zaro::edit::makeSetLut(
                window.project(), {sequence.id(), videoTrack.id()}, original.id, none);
            window.commands().execute(window.project(), std::move(*cleared));
            window.monitor()->update();
            QApplication::processEvents();
            const double off = meanGray(window.monitor()->grabFramebuffer());

            std::printf(
                "  look LUT: %.1f before, %.1f applied, %.1f at zero amount "
                "(a white frame reads %.1f)\n",
                plainDark, lifted, off, brightest);
            // The fixture lifts black to three quarters of white, so the lifted
            // frame should read most of what a white frame reads -- stated
            // against that frame rather than against a number.
            // A third of a white frame, not half: the measured ratio is about
            // 0.57, and a threshold sitting just under the value it checks is
            // a test that will fail for a reason nobody wants to investigate.
            if (!(lifted > brightest * 0.3) || !(lifted > plainDark + 1.0)) {
                std::fprintf(stderr, "  FAIL: the look LUT did not reach the picture\n");
                return 1;
            }
            if (std::fabs(off - plainDark) > 1.0) {
                std::fprintf(stderr, "  FAIL: an amount of zero still changed the picture\n");
                return 1;
            }

            while (window.commands().canUndo()) {
                window.commands().undo(window.project());
            }
            window.monitor()->update();
            QApplication::processEvents();
        }

        // The secondary, through its panel. The qualifier is tested headlessly
        // and compared against the shader; what neither of those can see is
        // whether these controls are connected to any of it.
        {
            auto* enable = window.effects()->findChild<QCheckBox*>("qualifier-enabled");
            auto* mask = window.effects()->findChild<QCheckBox*>("qualifier-show-mask");
            auto* lumaHigh = window.effects()->findChild<QDoubleSpinBox*>("qualifier-luma-high");
            if (enable == nullptr || mask == nullptr || lumaHigh == nullptr) {
                std::fprintf(stderr, "  FAIL: the qualifier controls are missing\n");
                return 1;
            }
            window.effects()->setSelection(videoTrack.id(), original.id);
            QApplication::processEvents();

            // A lit frame, so "selected" and "not selected" are a white mask
            // and a black one rather than two black pictures.
            std::int64_t litFrame = 0;
            double litness = 0.0;
            for (std::int64_t frame = 0; frame < 40; ++frame) {
                window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
                QApplication::processEvents();
                const double gray = meanGray(window.monitor()->grabFramebuffer());
                if (gray > litness) {
                    litness = gray;
                    litFrame = frame;
                }
            }
            window.setPosition(zaro::time::RationalTime{litFrame, sequence.frameRate()});
            QApplication::processEvents();
            // The picture itself, to compare the mask against. An absolute
            // threshold would be measuring the letterbox: how much of the
            // monitor the picture covers depends on the panel layout, and that
            // changes whenever a control is added.
            const double picture = meanGray(window.monitor()->grabFramebuffer());

            enable->setChecked(true);
            mask->setChecked(true);
            window.monitor()->update();
            QApplication::processEvents();
            const double everything = meanGray(window.monitor()->grabFramebuffer());

            // Now key only the darks. This frame is white, so it drops out.
            lumaHigh->setValue(0.2);
            window.monitor()->update();
            QApplication::processEvents();
            const double nothing = meanGray(window.monitor()->grabFramebuffer());

            std::printf("  qualifier mask: picture %.1f, wide open %.1f, keyed to darks %.1f\n",
                        picture, everything, nothing);
            // A white picture, entirely selected, shows as a white mask -- so
            // the two readings should agree.
            if (!(everything > picture * 0.85)) {
                std::fprintf(stderr,
                             "  FAIL: a qualifier left wide open did not select the picture "
                             "(%.1f against %.1f)\n",
                             everything, picture);
                return 1;
            }
            if (!(nothing < everything * 0.2)) {
                std::fprintf(stderr,
                             "  FAIL: narrowing the luma window did not change the mask; the "
                             "controls are not reaching the compositor\n");
                return 1;
            }

            while (window.commands().canUndo()) {
                window.commands().undo(window.project());
            }
            window.effects()->refresh();
            window.monitor()->update();
            QApplication::processEvents();
        }

        // The curve editor, driven with the mouse. The curve engine is tested
        // headlessly and the two render paths are compared against each other;
        // what neither of those can see is whether dragging in this widget
        // reaches any of it.
        {
            auto* editor = window.effects()->findChild<app::CurveEditor*>();
            if (editor == nullptr) {
                std::fprintf(stderr, "  FAIL: there is no curve editor\n");
                return 1;
            }
            window.effects()->setSelection(videoTrack.id(), original.id);
            QApplication::processEvents();

            const auto clipNow = [&]() {
                return window.project()
                    .findSequence(sequence.id())
                    ->videoTracks()
                    .front()
                    .find(original.id);
            };
            if (!clipNow()->curves.isIdentity()) {
                std::fprintf(stderr, "  FAIL: the clip starts with a curve on it\n");
                return 1;
            }

            // Grab the black point at the bottom-left and lift it. That is the
            // change this fixture can actually show: it is flashes on black, so
            // a midtone adjustment moves almost nothing, while lifting black
            // moves nearly every pixel.
            const QRect plot = editor->plotArea();
            const QPointF middle(plot.left(), plot.bottom());
            const QPointF lifted(middle.x(), middle.y() - (plot.height() * 0.4));
            {
                QMouseEvent press(QEvent::MouseButtonPress, middle, middle, Qt::LeftButton,
                                  Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(editor, &press);
                QMouseEvent move(QEvent::MouseMove, lifted, lifted, Qt::NoButton, Qt::LeftButton,
                                 Qt::NoModifier);
                QCoreApplication::sendEvent(editor, &move);
                QMouseEvent release(QEvent::MouseButtonRelease, lifted, lifted, Qt::LeftButton,
                                    Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(editor, &release);
            }
            QApplication::processEvents();

            const zaro::model::ToneCurve& master = clipNow()->curves.master;
            std::printf("  curve editor: %zu points, black lifted to %.3f\n", master.size(),
                        master.valueAt(0.0));
            if (master.size() < 2) {
                std::fprintf(stderr,
                             "  FAIL: the curve editor did not give the curve its endpoints\n");
                return 1;
            }
            if (!(master.valueAt(0.0) > 0.2)) {
                std::fprintf(stderr,
                             "  FAIL: dragging upward did not lift the curve; the widget's y "
                             "axis is inverted or it is not reaching the model\n");
                return 1;
            }
            if (master.points().front().x != 0.0) {
                std::fprintf(stderr,
                             "  FAIL: the black point moved sideways; the endpoints are supposed "
                             "to be pinned in x\n");
                return 1;
            }

            // And it has to reach the picture, not only the model. Measured on
            // a *dark* frame: this fixture is flashes on black, its lit frames
            // are saturated white, and lifting the black point cannot change
            // white at all. On a black frame the same lift moves every pixel.
            // Both extremes of the fixture. The bright one is the reference the
            // lift is judged against: an absolute threshold would be measuring
            // how much of the monitor the letterbox covers, which moves
            // whenever a panel is added -- it has already been wrong twice.
            std::int64_t darkFrame = 0;
            double darkness = 1e9;
            double brightest = 0.0;
            for (std::int64_t frame = 0; frame < 40; ++frame) {
                window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
                QApplication::processEvents();
                const double gray = meanGray(window.monitor()->grabFramebuffer());
                if (gray < darkness) {
                    darkness = gray;
                    darkFrame = frame;
                }
                brightest = std::max(brightest, gray);
            }
            window.setPosition(zaro::time::RationalTime{darkFrame, sequence.frameRate()});
            for (int i = 0; i < 3; ++i) {
                window.monitor()->update();
                QApplication::processEvents();
            }
            const double withCurve = meanGray(window.monitor()->grabFramebuffer());

            while (window.commands().canUndo()) {
                window.commands().undo(window.project());
            }
            for (int i = 0; i < 3; ++i) {
                window.monitor()->update();
                QApplication::processEvents();
            }
            const double withoutCurve = meanGray(window.monitor()->grabFramebuffer());
            std::printf("  curve on the GPU: %.1f with, %.1f without\n", withCurve, withoutCurve);
            if (!(withCurve > withoutCurve + 1.0)) {
                std::fprintf(stderr,
                             "  FAIL: the curve does not reach the preview; it would show on "
                             "export and not on screen\n");
                return 1;
            }
            window.effects()->refresh();
        }

        // Keyframing, driven through the panel and the timeline rather than by
        // calling the operations: the stopwatch, a value typed at a second
        // playhead position, and then dragging the diamond that appears.
        {
            auto* stopwatch = window.effects()->findChild<QToolButton*>("stopwatch:opacity");
            auto* keyButton = window.effects()->findChild<QToolButton*>("keyframe:opacity");
            if (stopwatch == nullptr || keyButton == nullptr) {
                std::fprintf(stderr, "  FAIL: the opacity stopwatch is missing\n");
                return 1;
            }

            window.effects()->setSelection(videoTrack.id(), original.id);
            window.setPosition(zaro::time::RationalTime{10, sequence.frameRate()});
            QApplication::processEvents();

            // The diamonds are the only sign in the timeline that a clip is
            // animated, and a painting bug there is invisible to every other
            // check. Counted inside the keyframe lane only: the clip names and
            // the ruler are drawn in almost the same near-white, and counting
            // the whole widget measures the text rather than the keyframes.
            const auto lanePixels = [&] {
                const QImage shot = timeline->grab().toImage();
                const auto row = timeline->rowFor(videoTrack.id());
                if (!row) {
                    return std::int64_t{-1};
                }
                const auto dpr = static_cast<int>(shot.devicePixelRatio());
                const int lane = timeline->layout().keyframeLaneHeight();
                const int top = (row->top + row->height - lane) * dpr;
                const int bottom = std::min((row->top + row->height) * dpr, shot.height());
                std::int64_t found = 0;
                for (int y = std::max(0, top); y < bottom; ++y) {
                    for (int x = 0; x < shot.width(); ++x) {
                        const QColor pixel = shot.pixelColor(x, y);
                        if (std::abs(pixel.red() - 226) <= 6 &&
                            std::abs(pixel.green() - 226) <= 6 &&
                            std::abs(pixel.blue() - 236) <= 6) {
                            ++found;
                        }
                    }
                }
                return found;
            };
            const std::int64_t bareLane = lanePixels();

            if (!stopwatch->isEnabled() || stopwatch->isChecked()) {
                std::fprintf(stderr, "  FAIL: the stopwatch is not offering to animate\n");
                return 1;
            }
            stopwatch->click();
            QApplication::processEvents();

            const auto clipNow = [&]() {
                return window.project()
                    .findSequence(sequence.id())
                    ->videoTracks()
                    .front()
                    .find(original.id);
            };
            const zaro::model::Curve* curve =
                clipNow()->animation.find(zaro::model::Param::Opacity);
            if (curve == nullptr || curve->size() != 1) {
                std::fprintf(stderr, "  FAIL: the stopwatch did not drop a keyframe\n");
                return 1;
            }
            if (!keyButton->isChecked()) {
                std::fprintf(stderr,
                             "  FAIL: the panel does not show a keyframe at the playhead\n");
                return 1;
            }

            // A second keyframe, made by typing a value at another position.
            window.setPosition(zaro::time::RationalTime{40, sequence.frameRate()});
            QApplication::processEvents();
            if (keyButton->isChecked()) {
                std::fprintf(stderr, "  FAIL: the panel claims a keyframe where there is none\n");
                return 1;
            }
            auto* opacitySpin = window.effects()
                                    ->findChild<QToolButton*>("keyframe:opacity")
                                    ->parentWidget()
                                    ->findChild<QDoubleSpinBox*>();
            opacitySpin->setValue(0.2);
            QApplication::processEvents();

            curve = clipNow()->animation.find(zaro::model::Param::Opacity);
            if (curve == nullptr || curve->size() != 2) {
                std::fprintf(stderr,
                             "  FAIL: typing a value while animated did not add a keyframe\n");
                return 1;
            }
            std::printf("  stopwatch and a typed value made %zu keyframes\n", curve->size());

            const std::int64_t drawn = lanePixels();
            std::printf("  keyframe diamonds cover %lld pixels in the lane (%lld before)\n",
                        static_cast<long long>(drawn), static_cast<long long>(bareLane));
            if (bareLane != 0) {
                std::fprintf(stderr,
                             "  FAIL: something else is painting in the lane, so this check "
                             "proves nothing\n");
                return 1;
            }
            if (drawn < 20) {
                std::fprintf(stderr, "  FAIL: the keyframes are not drawn on the timeline\n");
                return 1;
            }
            if (drawn < 20) {
                std::fprintf(stderr, "  FAIL: the keyframes are not drawn on the timeline\n");
                return 1;
            }

            // Drag the second diamond earlier, through the timeline.
            const auto keyRow = timeline->rowFor(videoTrack.id());
            const int laneY = keyRow->top + keyRow->height - 3;
            const int fromX = static_cast<int>(
                timeline->layout().xForTime(zaro::time::RationalTime{40, sequence.frameRate()}));
            const int toX = static_cast<int>(
                timeline->layout().xForTime(zaro::time::RationalTime{30, sequence.frameRate()}));
            dragOnTimeline(timeline, fromX, toX, laneY);
            QApplication::processEvents();

            curve = clipNow()->animation.find(zaro::model::Param::Opacity);
            // Where the pointer actually was, not where it was aimed: a frame
            // is wider than a pixel is precise, and asking for frame 30 by
            // pixel can legitimately land on 29.
            const zaro::time::RationalTime moved =
                clipNow()->sourceTimeAt(timeline->layout().timeForX(toX, sequence.frameRate()));
            if (curve == nullptr || curve->size() != 2 || curve->at(moved) == nullptr) {
                std::fprintf(stderr, "  FAIL: the keyframe did not follow the drag\n");
                return 1;
            }
            std::printf("  dragged a keyframe to source frame %lld, value %.2f\n",
                        static_cast<long long>(moved.frames()), curve->at(moved)->value);

            // Alt-click deletes one. The *first* keyframe, not the one just
            // dragged: it holds the same value as the static opacity, so
            // deleting it is what leaves the two different and makes the
            // stopwatch-off check below able to fail.
            {
                const int firstX = static_cast<int>(timeline->layout().xForTime(
                    zaro::time::RationalTime{10, sequence.frameRate()}));
                const QPointF where(firstX, laneY);
                QMouseEvent press(QEvent::MouseButtonPress, where, where, Qt::LeftButton,
                                  Qt::LeftButton, Qt::AltModifier);
                QCoreApplication::sendEvent(timeline, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, where, where, Qt::LeftButton,
                                    Qt::NoButton, Qt::AltModifier);
                QCoreApplication::sendEvent(timeline, &release);
            }
            QApplication::processEvents();
            curve = clipNow()->animation.find(zaro::model::Param::Opacity);
            if (curve == nullptr || curve->size() != 1) {
                std::fprintf(stderr, "  FAIL: alt-click did not delete the keyframe\n");
                return 1;
            }

            // And the stopwatch off again, keeping what was on screen.
            window.setPosition(zaro::time::RationalTime{10, sequence.frameRate()});
            QApplication::processEvents();
            const double showing =
                clipNow()->transformAt(zaro::time::RationalTime{10, sequence.frameRate()}).opacity;
            stopwatch->click();
            QApplication::processEvents();
            if (!clipNow()->animation.empty()) {
                std::fprintf(stderr, "  FAIL: the stopwatch did not stop animating\n");
                return 1;
            }
            if (std::fabs(showing - 1.0) < 1e-6) {
                std::fprintf(stderr,
                             "  FAIL: the animated value equals the static one, so this check "
                             "cannot tell them apart\n");
                return 1;
            }
            if (std::fabs(clipNow()->transform.opacity - showing) > 1e-6) {
                std::fprintf(stderr,
                             "  FAIL: turning animation off changed the picture (%.3f -> %.3f)\n",
                             showing, clipNow()->transform.opacity);
                return 1;
            }
            std::printf("  stopwatch off kept opacity at %.2f\n", clipNow()->transform.opacity);

            while (window.commands().canUndo()) {
                window.commands().undo(window.project());
            }
        }

        // Three-point editing, through the source monitor rather than by
        // calling the operation directly: mark a range, put the playhead
        // somewhere, and press the key.
        {
            const zaro::model::MediaRef& firstMedia = window.project().media().front();
            window.sourceMonitor()->load(firstMedia);
            window.sourceMonitor()->step(24);
            window.sourceMonitor()->markIn();
            window.sourceMonitor()->step(48);
            window.sourceMonitor()->markOut();
            QApplication::processEvents();

            const auto marked = window.sourceMonitor()->markedRange();
            if (!marked) {
                std::fprintf(stderr, "  FAIL: marking in and out produced no range\n");
                return 1;
            }
            std::printf("  marked %lld source frames\n",
                        static_cast<long long>(marked->duration().frames()));

            const auto& targetTrack =
                window.project().findSequence(sequence.id())->videoTracks().front();
            const std::size_t clipsBefore = targetTrack.clips().size();

            window.setPosition(zaro::time::RationalTime{5000, sequence.frameRate()});
            auto placed = zaro::edit::makePlaceFromSource(
                window.project(), {sequence.id(), targetTrack.id()},
                window.sourceMonitor()->media(), *marked,
                zaro::time::RationalTime{5000, sequence.frameRate()},
                zaro::edit::PlaceMode::Overwrite);
            if (!placed) {
                std::fprintf(stderr, "  FAIL: %s\n", placed.error().toString().c_str());
                return 1;
            }
            window.commands().execute(window.project(), std::move(*placed));

            const auto& trackAfter =
                window.project().findSequence(sequence.id())->videoTracks().front();
            if (trackAfter.clips().size() != clipsBefore + 1) {
                std::fprintf(stderr, "  FAIL: the three-point edit added no clip\n");
                return 1;
            }
            const zaro::model::Clip* placedClip =
                trackAfter.clipAt(zaro::time::RationalTime{5000, sequence.frameRate()});
            if (placedClip == nullptr) {
                std::fprintf(stderr, "  FAIL: nothing landed at the playhead\n");
                return 1;
            }
            std::printf("  placed %lld frames at the playhead\n",
                        static_cast<long long>(placedClip->duration().frames()));
            if (placedClip->duration().frames() <= 0) {
                std::fprintf(stderr, "  FAIL: the placed clip has no duration\n");
                return 1;
            }
        }

        // Multi-selection, driven as a rubber band. Starting below the last
        // track means the press lands on empty timeline rather than grabbing a
        // clip, which is what makes it a band rather than a move.
        {
            const auto* seq = window.project().findSequence(sequence.id());
            const auto lastRow = timeline->rowFor(seq->audioTracks().back().id());
            if (!lastRow) {
                std::fprintf(stderr, "  FAIL: no audio row\n");
                return 1;
            }
            const int belowTracks = lastRow->top + lastRow->height + 12;
            const int acrossTop = timeline->rowFor(seq->videoTracks().front().id())->top + 4;

            std::int64_t clipsBefore = 0;
            for (const auto* list : {&seq->videoTracks(), &seq->audioTracks()}) {
                for (const auto& track : *list) {
                    clipsBefore += static_cast<std::int64_t>(track.clips().size());
                }
            }

            {
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(200, belowTracks),
                                  QPointF(200, belowTracks), Qt::LeftButton, Qt::LeftButton,
                                  Qt::NoModifier);
                QCoreApplication::sendEvent(timeline, &press);
                // Baseline after the press, not before: the press clears the
                // previous selection, and those clips repaint. Taking it here
                // leaves the band as the only difference.
                const QImage quiet = timeline->grab().toImage();
                for (int i = 1; i <= 8; ++i) {
                    const int bx = 200 + (1400 - 200) * i / 8;
                    const int by = belowTracks + (acrossTop - belowTracks) * i / 8;
                    QMouseEvent move(QEvent::MouseMove, QPointF(bx, by), QPointF(bx, by),
                                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(timeline, &move);
                }
                // The band rectangle is the only feedback this gesture gives,
                // so check it actually reaches the screen, and only inside its
                // own bounds.
                const QImage banded = timeline->grab().toImage();
                // grab() returns device pixels, so on a retina display the
                // image is twice the size of the coordinates the events used.
                const auto dpr = static_cast<int>(banded.devicePixelRatio());
                const QRect drawn = QRect(QPoint(200, belowTracks), QPoint(1400, acrossTop))
                                        .normalized()
                                        .intersected(timeline->rect());
                std::int64_t changedInside = 0;
                std::int64_t changedOutside = 0;
                for (int py = 0; py < banded.height(); ++py) {
                    for (int px = 0; px < banded.width(); ++px) {
                        if (banded.pixel(px, py) == quiet.pixel(px, py)) {
                            continue;
                        }
                        if (drawn.adjusted(-2, -2, 2, 2).contains(px / dpr, py / dpr)) {
                            ++changedInside;
                        } else {
                            ++changedOutside;
                        }
                    }
                }
                std::printf("  band drawn: %lld pixels changed inside, %lld outside\n",
                            static_cast<long long>(changedInside),
                            static_cast<long long>(changedOutside));
                if (changedInside == 0) {
                    std::fprintf(stderr, "  FAIL: the rubber band painted nothing\n");
                    return 1;
                }
                if (changedOutside > 0) {
                    std::fprintf(stderr, "  FAIL: the band painted outside its own rectangle\n");
                    return 1;
                }

                QMouseEvent release(QEvent::MouseButtonRelease, QPointF(1400, acrossTop),
                                    QPointF(1400, acrossTop), Qt::LeftButton, Qt::NoButton,
                                    Qt::NoModifier);
                QCoreApplication::sendEvent(timeline, &release);
            }

            // Dragging any member of a selection has to move all of it, by the
            // same amount. Record where everything sits, nudge one clip, and
            // compare the shifts.
            std::vector<std::pair<model::ClipId, std::int64_t>> startsBefore;
            for (const auto* list : {&seq->videoTracks(), &seq->audioTracks()}) {
                for (const auto& track : *list) {
                    for (const auto& clip : track.clips()) {
                        startsBefore.emplace_back(clip.id, clip.start().frames());
                    }
                }
            }
            const auto videoRow = timeline->rowFor(seq->videoTracks().front().id());
            const int grabY = videoRow->top + videoRow->height / 2;
            dragOnTimeline(timeline, 400, 520, grabY);

            const auto* moved = window.project().findSequence(sequence.id());
            std::int64_t shifted = 0;
            std::int64_t commonShift = 0;
            for (const auto* list : {&moved->videoTracks(), &moved->audioTracks()}) {
                for (const auto& track : *list) {
                    for (const auto& clip : track.clips()) {
                        for (const auto& [id, was] : startsBefore) {
                            if (id != clip.id) {
                                continue;
                            }
                            const std::int64_t delta = clip.start().frames() - was;
                            if (delta == 0) {
                                continue;
                            }
                            ++shifted;
                            if (commonShift == 0) {
                                commonShift = delta;
                            } else if (commonShift != delta) {
                                std::fprintf(stderr,
                                             "  FAIL: selection moved unevenly (%lld vs %lld)\n",
                                             static_cast<long long>(commonShift),
                                             static_cast<long long>(delta));
                                shifted = -1;
                            }
                        }
                    }
                }
            }
            if (shifted < 2) {
                std::fprintf(stderr, "  FAIL: dragging one selected clip moved %lld clips\n",
                             static_cast<long long>(shifted));
                return 1;
            }
            std::printf("  dragging one member moved %lld clips by %lld frames each\n",
                        static_cast<long long>(shifted), static_cast<long long>(commonShift));
            while (window.commands().canUndo()) {
                window.commands().undo(window.project());
            }

            // Deleting reports how much the band caught.
            QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
            QCoreApplication::sendEvent(timeline, &del);

            const auto* banded = window.project().findSequence(sequence.id());
            std::int64_t clipsAfter = 0;
            for (const auto* list : {&banded->videoTracks(), &banded->audioTracks()}) {
                for (const auto& track : *list) {
                    clipsAfter += static_cast<std::int64_t>(track.clips().size());
                }
            }
            std::printf("  rubber band removed %lld of %lld clips\n",
                        static_cast<long long>(clipsBefore - clipsAfter),
                        static_cast<long long>(clipsBefore));
            if (clipsBefore - clipsAfter < 2) {
                std::fprintf(stderr,
                             "  FAIL: the band selected fewer than two clips across tracks\n");
                return 1;
            }

            window.commands().undo(window.project());
        }

        // Multicam, switched with the keyboard the way it is actually cut:
        // watch it play and press the camera you want. The angles and the cut
        // are tested headlessly; what that cannot show is whether the key
        // reaches the edit.
        {
            const auto outerSequenceId = sequence.id();
            const auto trackId = videoTrack.id();
            const auto mediaId = window.project().media().front().id;

            zaro::model::Clip::Angle a;
            a.media = mediaId;
            a.offset = zaro::time::RationalTime{0, sequence.frameRate()};
            a.name = "A";
            zaro::model::Clip::Angle b;
            b.media = mediaId;
            // A different point in the same file stands in for a second camera:
            // what is being checked is the switch, not the footage.
            b.offset = zaro::time::RationalTime{50, sequence.frameRate()};
            b.name = "B";

            auto built = zaro::edit::makeMulticam(
                window.project(), {outerSequenceId, trackId}, {a, b},
                zaro::time::TimeRange{zaro::time::RationalTime{0, sequence.frameRate()},
                                      zaro::time::RationalTime{60, sequence.frameRate()}});
            if (!built) {
                std::fprintf(stderr, "  FAIL: %s\n", built.error().toString().c_str());
                return 1;
            }
            window.commands().execute(window.project(), std::move(*built));

            const auto clipsOn = [&]() {
                return window.project()
                    .findSequence(outerSequenceId)
                    ->findTrack(trackId)
                    ->clips()
                    .size();
            };
            const std::size_t clipsBeforeSwitch = clipsOn();

            // Select it, put the playhead inside it, and press 2.
            const auto id = window.project()
                                .findSequence(outerSequenceId)
                                ->findTrack(trackId)
                                ->clips()
                                .front()
                                .id;
            window.effects()->setSelection(trackId, id);
            timeline->selectOnlyForTest(trackId, id);
            window.setPosition(zaro::time::RationalTime{25, sequence.frameRate()});
            QApplication::processEvents();

            QKeyEvent two(QEvent::KeyPress, Qt::Key_2, Qt::NoModifier);
            QCoreApplication::sendEvent(timeline, &two);
            QApplication::processEvents();

            const std::size_t clipsAfterSwitch = clipsOn();
            std::printf("  multicam: %zu clips before the switch, %zu after\n", clipsBeforeSwitch,
                        clipsAfterSwitch);
            if (clipsAfterSwitch != clipsBeforeSwitch + 1) {
                std::fprintf(stderr, "  FAIL: switching an angle did not cut the clip\n");
                return 1;
            }
            const auto& clips =
                window.project().findSequence(outerSequenceId)->findTrack(trackId)->clips();
            if (clips[1].activeAngle != 1 || clips[1].start().frames() != 25) {
                std::fprintf(stderr,
                             "  FAIL: the cut is in the wrong place or on the wrong angle\n");
                return 1;
            }

            while (window.commands().canUndo()) {
                window.commands().undo(window.project());
            }
            window.monitor()->update();
            QApplication::processEvents();
        }

        // Second to last: this adds a track, and adding one reallocates the
        // sequence's vector of them -- so the reference this function has
        // held since the top must be finished with by now.
        // Syncing multicam angles, through the real window.
        //
        // Timecode rather than by ear: the arithmetic of both is tested
        // headlessly, and what this has to show is that picking a clip, asking
        // for a sync and having the offsets land on it works end to end. By
        // ear would also mean reading a minute of audio from a ten-second
        // fixture, which is a test of silence handling, not of syncing.
        {
            const auto syncSequenceId = window.project().activeSequence();
            const auto syncTrackId =
                window.project().findSequence(syncSequenceId)->videoTracks().front().id();
            const auto syncRate = window.project().findSequence(syncSequenceId)->frameRate();

            // A second reference to the same file stands in for a second
            // camera: what is being checked is the sync, not the footage. Two
            // references so that each can carry its own start timecode, which
            // is what a second camera would have.
            zaro::model::MediaRef cameraB = window.project().media().front();
            cameraB.id = window.project().ids().next<zaro::model::MediaRefTag>();
            cameraB.name = "cam-b";
            const auto cameraBId = window.project().addMedia(cameraB);

            const auto stamp = [&](zaro::model::MediaRefId id, const char* text) {
                for (zaro::model::MediaRef& media : window.project().mediaMutable()) {
                    if (media.id == id && !media.info.videoStreams.empty()) {
                        media.info.videoStreams.front().startTimecode =
                            zaro::time::parseTimecode(text);
                    }
                }
            };
            stamp(window.project().media().front().id, "01:00:00:00");
            // Rolled two seconds later on the same clock.
            stamp(cameraBId, "01:00:02:00");
            if (Status reopened = window.reopenMedia(); !reopened) {
                std::fprintf(stderr, "  FAIL: %s\n", reopened.error().toString().c_str());
                return 1;
            }

            zaro::model::Clip::Angle angleA;
            angleA.media = window.project().media().front().id;
            angleA.name = "A";
            zaro::model::Clip::Angle angleB;
            angleB.media = cameraBId;
            // Deliberately wrong to begin with, so that a sync which does
            // nothing at all cannot pass.
            angleB.offset = zaro::time::RationalTime{99, syncRate};
            angleB.name = "B";

            auto placed = zaro::edit::makeMulticam(
                window.project(), {syncSequenceId, syncTrackId}, {angleA, angleB},
                zaro::time::TimeRange{zaro::time::RationalTime{0, syncRate},
                                      zaro::time::RationalTime{60, syncRate}});
            if (!placed) {
                std::fprintf(stderr, "  FAIL: %s\n", placed.error().toString().c_str());
                return 1;
            }
            window.commands().execute(window.project(), std::move(*placed));

            const auto syncClipId = window.project()
                                        .findSequence(syncSequenceId)
                                        ->findTrack(syncTrackId)
                                        ->clips()
                                        .front()
                                        .id;
            timeline->selectOnlyForTest(syncTrackId, syncClipId);
            QApplication::processEvents();

            window.syncAngles(/*byEar=*/false);
            QApplication::processEvents();

            const auto* syncedClip = window.project()
                                         .findSequence(syncSequenceId)
                                         ->findTrack(syncTrackId)
                                         ->find(syncClipId);
            if (syncedClip == nullptr) {
                std::fprintf(stderr, "  FAIL: the multicam clip went missing\n");
                return 1;
            }
            const double offsetSeconds = syncedClip->angles[1].offset.toSeconds().toDouble();
            std::printf("  multicam sync: %d angles synced, %d skipped, camera B at %+.2fs\n",
                        window.lastSyncCount(), window.lastSyncSkipped(), offsetSeconds);
            if (window.lastSyncCount() != 2 || window.lastSyncSkipped() != 0) {
                std::fprintf(stderr, "  FAIL: the sync did not reach both angles\n");
                return 1;
            }
            // Two seconds later on the clock means reading two seconds *less*
            // far into that camera's own material for the same moment.
            if (std::abs(offsetSeconds + 2.0) > 0.05) {
                std::fprintf(stderr, "  FAIL: camera B was put at %+.2fs, not -2.00s\n",
                             offsetSeconds);
                return 1;
            }

            // And it is one undoable step: the wrong offset comes back whole.
            window.commands().undo(window.project());
            const auto* undone = window.project()
                                     .findSequence(syncSequenceId)
                                     ->findTrack(syncTrackId)
                                     ->find(syncClipId);
            if (undone == nullptr || undone->angles[1].offset.frames() != 99) {
                std::fprintf(stderr, "  FAIL: undoing the sync did not restore the offset\n");
                return 1;
            }

            while (window.commands().canUndo()) {
                window.commands().undo(window.project());
            }
            // Put the project back as it was found. Adding a media reference is
            // not a command, so nothing above undoes it, and the blocks that
            // follow are entitled to the project they were written against.
            window.project().mediaMutable().pop_back();
            window.project().mediaMutable().front().info.videoStreams.front().startTimecode =
                std::nullopt;
            if (Status reopened = window.reopenMedia(); !reopened) {
                std::fprintf(stderr, "  FAIL: %s\n", reopened.error().toString().c_str());
                return 1;
            }
            window.monitor()->update();
            QApplication::processEvents();
        }

        // An adjustment layer, through the real preview. This is the one
        // feature where the GPU path deliberately hands the whole frame to the
        // CPU compositor, so what this checks is that the fallback is wired and
        // that what it produces reaches the screen.
        {
            const auto adjustSequenceId = sequence.id();
            // This fixture has one video track, and an adjustment layer needs
            // something to sit above.
            if (window.project().findSequence(adjustSequenceId)->videoTracks().size() < 2) {
                auto added = zaro::edit::makeAddTrack(window.project(), adjustSequenceId,
                                                      zaro::model::TrackKind::Video, "V2");
                if (!added) {
                    std::fprintf(stderr, "  FAIL: %s\n", added.error().toString().c_str());
                    return 1;
                }
                window.commands().execute(window.project(), std::move(*added));
            }
            const auto aboveId =
                window.project().findSequence(adjustSequenceId)->videoTracks()[1].id();

            std::int64_t brightestFrame = 0;
            double brightest = 0.0;
            for (std::int64_t frame = 0; frame < 40; ++frame) {
                window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
                QApplication::processEvents();
                const double gray = meanGray(window.monitor()->grabFramebuffer());
                if (gray > brightest) {
                    brightest = gray;
                    brightestFrame = frame;
                }
            }
            window.setPosition(zaro::time::RationalTime{brightestFrame, sequence.frameRate()});
            QApplication::processEvents();
            const double plain = meanGray(window.monitor()->grabFramebuffer());

            auto built = zaro::edit::makeAddAdjustment(
                window.project(), {adjustSequenceId, aboveId},
                zaro::time::TimeRange{zaro::time::RationalTime{0, sequence.frameRate()},
                                      zaro::time::RationalTime{60, sequence.frameRate()}});
            if (!built) {
                std::fprintf(stderr, "  FAIL: %s\n", built.error().toString().c_str());
                return 1;
            }
            window.commands().execute(window.project(), std::move(*built));
            const auto layerId = window.project()
                                     .findSequence(adjustSequenceId)
                                     ->findTrack(aboveId)
                                     ->clips()
                                     .front()
                                     .id;

            zaro::model::ColorCorrection darker;
            darker.exposure = -2.0;
            auto graded = zaro::edit::makeSetColorCorrection(
                window.project(), {adjustSequenceId, aboveId}, layerId, darker);
            if (!graded) {
                std::fprintf(stderr, "  FAIL: %s\n", graded.error().toString().c_str());
                return 1;
            }
            window.commands().execute(window.project(), std::move(*graded));
            window.monitor()->update();
            QApplication::processEvents();
            const double adjusted = meanGray(window.monitor()->grabFramebuffer());

            std::printf("  adjustment layer: %.1f plain, %.1f two stops down from above\n", plain,
                        adjusted);
            if (!(adjusted < plain * 0.5)) {
                std::fprintf(stderr, "  FAIL: the adjustment layer did not reach the preview\n");
                return 1;
            }

            while (window.commands().canUndo()) {
                window.commands().undo(window.project());
            }
            window.monitor()->update();
            QApplication::processEvents();
        }

        // The render cache, through the real window.
        //
        // The cache only ever answers the CPU compositor, so this needs a
        // sequence the preview cannot draw on the GPU -- which is exactly what
        // an adjustment layer is. Rendered ahead, then played: what is checked
        // is that the frames the monitor asks for come back from the cache
        // rather than being composited again, and that an edit under them stops
        // that happening.
        {
            const auto cacheSequenceId = window.project().activeSequence();
            if (window.project().findSequence(cacheSequenceId)->videoTracks().size() < 2) {
                auto added = zaro::edit::makeAddTrack(window.project(), cacheSequenceId,
                                                      zaro::model::TrackKind::Video, "V2");
                if (!added) {
                    std::fprintf(stderr, "  FAIL: %s\n", added.error().toString().c_str());
                    return 1;
                }
                window.commands().execute(window.project(), std::move(*added));
            }
            const auto cacheRate = window.project().findSequence(cacheSequenceId)->frameRate();
            const auto cacheTrackId =
                window.project().findSequence(cacheSequenceId)->videoTracks()[1].id();

            auto layer = zaro::edit::makeAddAdjustment(
                window.project(), {cacheSequenceId, cacheTrackId},
                zaro::time::TimeRange{zaro::time::RationalTime{0, cacheRate},
                                      zaro::time::RationalTime{40, cacheRate}});
            if (!layer) {
                std::fprintf(stderr, "  FAIL: %s\n", layer.error().toString().c_str());
                return 1;
            }
            window.commands().execute(window.project(), std::move(*layer));
            const auto cacheLayerId = window.project()
                                          .findSequence(cacheSequenceId)
                                          ->findTrack(cacheTrackId)
                                          ->clips()
                                          .front()
                                          .id;

            window.timeline()->zoomToFit();
            QApplication::processEvents();
            const auto covers = [](const std::vector<zaro::time::TimeRange>& spans,
                                   std::int64_t frame) {
                for (const zaro::time::TimeRange& span : spans) {
                    if (frame >= span.start().frames() && frame < span.endExclusive().frames()) {
                        return true;
                    }
                }
                return false;
            };

            window.renderCache().clear();
            window.renderVisibleRange();
            const std::size_t cached = window.renderCache().count();
            const std::size_t spans = window.timeline()->cachedSpans().size();
            if (cached == 0 || spans == 0 || !covers(window.timeline()->cachedSpans(), 10)) {
                std::fprintf(stderr, "  FAIL: pre-rendering cached %zu frames in %zu spans\n",
                             cached, spans);
                return 1;
            }

            // Play it. Every frame the monitor draws over this range should
            // already be in the cache.
            window.renderCache().resetStatistics();
            for (std::int64_t frame = 0; frame < 20; ++frame) {
                window.setPosition(zaro::time::RationalTime{frame, cacheRate});
                QApplication::processEvents();
            }
            const auto hits = window.renderCache().hits();
            const auto misses = window.renderCache().misses();
            std::printf(
                "  render cache: %zu frames pre-rendered, %llu hits and %llu misses "
                "playing them back\n",
                cached, static_cast<unsigned long long>(hits),
                static_cast<unsigned long long>(misses));
            if (hits == 0 || misses > hits) {
                std::fprintf(stderr, "  FAIL: the preview did not read from the render cache\n");
                return 1;
            }

            // And an edit under the bar takes it away. Not "eventually", and
            // not by anyone remembering to say so: the frame is made of
            // something that has changed, so it stops being a frame.
            zaro::model::ColorCorrection darker;
            darker.exposure = -2.0;
            auto graded = zaro::edit::makeSetColorCorrection(
                window.project(), {cacheSequenceId, cacheTrackId}, cacheLayerId, darker);
            if (!graded) {
                std::fprintf(stderr, "  FAIL: %s\n", graded.error().toString().c_str());
                return 1;
            }
            window.commands().execute(window.project(), std::move(*graded));
            // What the timeline's `edited` signal calls. Driven directly here
            // because this edit was made against the model rather than through
            // the widget that emits it.
            window.updateCacheBar();
            QApplication::processEvents();
            if (covers(window.timeline()->cachedSpans(), 10)) {
                std::fprintf(stderr, "  FAIL: the cache bar survived an edit under it\n");
                return 1;
            }
            // And only where the edit reaches: the layer stops at frame 40, so
            // the rest of the timeline is still rendered. A cache that threw
            // everything away on every edit would pass the check above and be
            // useless.
            if (!covers(window.timeline()->cachedSpans(), 200)) {
                std::fprintf(stderr, "  FAIL: an edit over 40 frames cleared the whole bar\n");
                return 1;
            }

            while (window.commands().canUndo()) {
                window.commands().undo(window.project());
            }
            window.renderCache().clear();
            window.monitor()->update();
            QApplication::processEvents();
        }

        // Last on purpose. Adding a sequence reallocates the project's
        // vector of them, so the reference this function has held since the
        // top dangles from here on -- and everything else is done by now.
        // A nested sequence, through the real preview. The recursion and the
        // cycle refusal are tested headlessly; what those cannot show is
        // whether a nest reaches the screen, which on the GPU path means a CPU
        // composite and an upload rather than the ordinary route.
        {
            // Captured by value first: adding a sequence reallocates the
            // project's vector, and the reference this self-test has been
            // holding since the top would dangle. It did, and the first run
            // aborted inside rational arithmetic reading the wreckage.
            const auto outerId = window.project().activeSequence();
            const auto outerRate = window.project().findSequence(outerId)->frameRate();
            const auto outerTrackId =
                window.project().findSequence(outerId)->videoTracks().front().id();

            // An inner sequence holding a white rectangle over its whole
            // length, so what it contributes is unmistakable.
            zaro::model::Sequence inner{window.project().ids().next<zaro::model::SequenceTag>(),
                                        "nested", outerRate};
            inner.setSize(window.sequence()->width(), window.sequence()->height());
            const auto innerId = inner.id();
            const auto innerTrack = window.project().ids().next<zaro::model::TrackTag>();
            inner.addTrack(innerTrack, zaro::model::TrackKind::Video, "V1");
            window.project().addSequence(std::move(inner));
            window.rebindSequence();

            zaro::model::Graphic block;
            block.kind = zaro::model::GraphicKind::Rectangle;
            block.width = 4000.0;
            block.height = 4000.0;
            auto filled = zaro::edit::makeAddGraphic(
                window.project(), {innerId, innerTrack}, block,
                zaro::time::TimeRange{zaro::time::RationalTime{0, outerRate},
                                      zaro::time::RationalTime{40, outerRate}});
            if (!filled) {
                std::fprintf(stderr, "  FAIL: %s\n", filled.error().toString().c_str());
                return 1;
            }
            window.commands().execute(window.project(), std::move(*filled));

            std::int64_t darkFrame = 0;
            double darkest = 1e9;
            for (std::int64_t frame = 0; frame < 35; ++frame) {
                window.setPosition(zaro::time::RationalTime{frame, outerRate});
                QApplication::processEvents();
                const double gray = meanGray(window.monitor()->grabFramebuffer());
                if (gray < darkest) {
                    darkest = gray;
                    darkFrame = frame;
                }
            }
            window.setPosition(zaro::time::RationalTime{darkFrame, outerRate});
            QApplication::processEvents();
            const double without = meanGray(window.monitor()->grabFramebuffer());

            auto nested =
                zaro::edit::makeNestSequence(window.project(), {outerId, outerTrackId}, innerId,
                                             zaro::time::RationalTime{0, outerRate});
            if (!nested) {
                std::fprintf(stderr, "  FAIL: %s\n", nested.error().toString().c_str());
                return 1;
            }
            window.commands().execute(window.project(), std::move(*nested));
            window.monitor()->update();
            QApplication::processEvents();
            const double withNest = meanGray(window.monitor()->grabFramebuffer());

            std::printf("  nesting: %.1f with a nested sequence, %.1f without\n", withNest,
                        without);
            if (!(withNest > without + 50.0)) {
                std::fprintf(stderr, "  FAIL: the nested sequence did not reach the preview\n");
                return 1;
            }

            // And a sequence still cannot contain itself.
            if (zaro::edit::makeNestSequence(window.project(), {outerId, outerTrackId}, outerId,
                                             zaro::time::RationalTime{0, outerRate})) {
                std::fprintf(stderr, "  FAIL: a sequence was allowed inside itself\n");
                return 1;
            }

            while (window.commands().canUndo()) {
                window.commands().undo(window.project());
            }
            window.monitor()->update();
            QApplication::processEvents();
        }

        std::printf("  ok\n");
        return 0;
    }

    if (!selfTest) {
        return QApplication::exec();
    }

    // Prove a picture actually reached the widget, rather than that the window
    // opened. Renders a handful of frames across the sequence and reads one
    // back -- the only readback in this program, and it exists for this check.
    const zaro::model::Sequence& sequence = *window.sequence();
    const std::int64_t last = std::max<std::int64_t>(0, sequence.duration().frames() - 1);
    window.waitForWaveforms();
    // Sample across the sequence and keep the brightest. A single position is
    // not a fair test: plenty of real footage is legitimately black at any
    // given moment, and a fixture that is black except on flash frames would
    // fail a check aimed at one timecode.
    QImage grabbed;
    double bestLit = 0.0;
    for (int i = 0; i < 5; ++i) {
        window.setPosition(zaro::time::RationalTime{last * i / 5, sequence.frameRate()});
        QApplication::processEvents();
        const QImage shot = window.monitor()->grabFramebuffer();
        if (shot.isNull()) {
            continue;
        }
        std::int64_t lit = 0;
        for (int y = 0; y < shot.height(); ++y) {
            for (int x = 0; x < shot.width(); ++x) {
                if (qGray(shot.pixel(x, y)) > 8) {
                    ++lit;
                }
            }
        }
        const double fraction =
            static_cast<double>(lit) / static_cast<double>(shot.width() * shot.height());
        if (grabbed.isNull() || fraction > bestLit) {
            bestLit = fraction;
            grabbed = shot;
        }
    }

    if (!window.monitor()->lastError().isEmpty()) {
        std::fprintf(stderr, "zaro-preview: %s\n",
                     window.monitor()->lastError().toUtf8().constData());
        return 1;
    }
    if (grabbed.isNull()) {
        std::fprintf(stderr, "zaro-preview: the monitor produced no image\n");
        return 1;
    }

    // Black at every sampled position would mean the pipeline ran and drew
    // nothing, which is the failure this is really looking for.
    const double litFraction = bestLit;

    std::printf("zaro-preview selftest\n");
    std::printf("  %lld frames rendered through the widget\n",
                static_cast<long long>(window.monitor()->framesRendered()));
    std::printf("  grabbed %dx%d, %.1f%% of it lit\n", grabbed.width(), grabbed.height(),
                litFraction * 100.0);

    if (!capturePath.isEmpty()) {
        // The whole window, so the timeline is in the picture too.
        const QPixmap windowShot = window.grab();
        if (!windowShot.isNull()) {
            windowShot.save(capturePath + ".window.png");
        }
        if (grabbed.save(capturePath)) {
            std::printf("  saved %s\n", capturePath.toUtf8().constData());
        } else {
            std::fprintf(stderr, "  FAIL: cannot write %s\n", capturePath.toUtf8().constData());
            return 1;
        }
    }

    if (litFraction < 0.05) {
        std::fprintf(stderr, "  FAIL: the monitor is essentially black\n");
        return 1;
    }
    std::printf("  ok\n");
    return 0;
}
