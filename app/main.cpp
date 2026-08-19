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
#include <QDoubleSpinBox>
#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
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
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"
#include "zaro/core/playback/Transport.h"
#include "zaro/core/render/AudioGraph.h"
#include "zaro/core/render/RenderGraph.h"
#include "zaro/core/render/Scopes.h"
#include "zaro/core/time/Timecode.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"
#include "zaro/platform/sdl/AudioSink.h"

#include "CurveEditor.h"
#include "EffectControls.h"
#include "ExportDialog.h"
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
        sequence_ = project_.findSequence(project_.activeSequence());

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
        rightColumn->setStretchFactor(0, 2);
        rightColumn->setStretchFactor(1, 1);
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
            scrubber_->setRange(0, static_cast<int>(sequence_->duration().frames()));
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
        connect(scopes_, &app::ScopesPanel::measurementNeeded, this, [this] { measureScopes(); });
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
        connect(timeline_, &app::TimelineWidget::edited, this, [this] {
            // Undo can change a clip's parameters as well as its position, so
            // the panel has to re-read rather than trust what it last wrote.
            effects_->refresh();
            // An edit can change the duration, and can change what is under the
            // playhead, so both the scrubber and the picture need refreshing.
            scrubber_->setRange(0, static_cast<int>(sequence_->duration().frames()));
            monitor_->update();
            refresh();
        });

        connect(playButton_, &QPushButton::clicked, this, [this] { togglePlay(); });
        connect(scrubber_, &QSlider::sliderMoved, this, [this](int value) {
            // Scrubbing stops playback: the playhead is being driven by hand,
            // and having the clock fight it is what makes scrubbing feel loose.
            stop();
            setPosition(time::RationalTime{value, sequence_->frameRate()});
        });

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
    [[nodiscard]] const model::Sequence* sequence() const { return sequence_; }
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
    [[nodiscard]] edit::CommandStack& commands() { return commands_; }

    Status openMedia() {
        auto opened = platform::ffmpeg::ProjectMediaSource::open(project_);
        if (!opened) {
            return opened.error();
        }
        media_ = std::move(*opened);
        monitor_->setSource(sequence_, media_.get());
        timeline_->setProject(&project_, sequence_->id(), &commands_);
        effects_->setProject(&project_, sequence_->id(), &commands_);
        bin_->setProject(&project_, sequence_->id(), &commands_);
        source_->setProvider(media_.get());
        startWaveforms();
        scrubber_->setRange(0, static_cast<int>(sequence_->duration().frames()));
        refresh();
        return {};
    }

    void setPosition(const time::RationalTime& position) {
        const std::int64_t last = std::max<std::int64_t>(0, sequence_->duration().frames() - 1);
        const std::int64_t clamped = std::clamp<std::int64_t>(position.frames(), 0, last);
        position_ = time::RationalTime{clamped, sequence_->frameRate()};
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
        setPosition(position_ + time::RationalTime{frames, sequence_->frameRate()});
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
                setPosition(time::RationalTime{0, sequence_->frameRate()});
                return;
            case Qt::Key_End:
                stop();
                setPosition(sequence_->duration());
                return;
            case Qt::Key_E:
                if (event->modifiers().testFlag(Qt::ControlModifier) ||
                    event->modifiers().testFlag(Qt::MetaModifier)) {
                    stop();
                    app::ExportDialog dialog{project_, sequence_->id(), this};
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
        if (const model::Marker* marker = sequence_->markerAfter(position_)) {
            stop();
            setPosition(marker->range.start());
        }
    }
    void doPreviousMarker() {
        if (const model::Marker* marker = sequence_->markerBefore(position_)) {
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
        const auto& videoTracks = sequence_->videoTracks();
        if (videoTracks.empty()) {
            return;
        }
        auto built =
            edit::makePlaceFromSource(project_, {sequence_->id(), videoTracks.front().id()},
                                      source_->media(), *range, position_, mode);
        if (!built) {
            return;
        }
        commands_.execute(project_, std::move(*built));
        commands_.breakMerge();
        scrubber_->setRange(0, static_cast<int>(sequence_->duration().frames()));
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
        const time::Rational& audioRate = sequence_->audioSampleRate();
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
        const time::Rational& audioRate = sequence_->audioSampleRate();
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
                    if (auto mixed = mixer.mix(*sequence_, from, block, kChannels)) {
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
        const time::Rational& audioRate = sequence_->audioSampleRate();
        const std::int64_t clock = sink_ ? sink_->clockFrames() : 0;

        // Position is an exact rational function of elapsed audio, floored to
        // the frame the playhead is inside -- the same arithmetic the scheduler
        // uses, and for the same reason.
        const time::Rational elapsed = time::Rational{clock - anchorClock_, 1} / audioRate;
        const time::Rational advanced = elapsed * transport_.speed() * sequence_->frameRate();
        const auto position =
            anchorPosition_ + time::RationalTime{advanced.floorToInt(), sequence_->frameRate()};

        if (position.frames() >= sequence_->duration().frames() || position.frames() < 0) {
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
        if (scopes_ == nullptr || media_ == nullptr || sequence_ == nullptr) {
            return;
        }
        if (playing_ || !scopes_->wantsMeasurement()) {
            return;
        }
        render::RenderGraph graph{*media_};
        auto frame = graph.composite(*sequence_, position_);
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

    void refresh() {
        if (sequence_ == nullptr) {
            return;
        }
        const bool dropFrame = time::supportsDropFrame(sequence_->frameRate());
        const time::Timecode code =
            time::timecodeFromFrames(position_.frames(), sequence_->frameRate(), dropFrame);
        timecode_->setText(QString::fromStdString(code.toString()));
        if (!scrubber_->isSliderDown()) {
            scrubber_->setValue(static_cast<int>(position_.frames()));
        }
    }

    model::Project project_;
    io::LoadedProject loaded_;
    const model::Sequence* sequence_{nullptr};
    std::unique_ptr<platform::ffmpeg::ProjectMediaSource> media_;
    std::unique_ptr<platform::sdl::AudioSink> sink_;

    app::ProgramMonitor* monitor_{nullptr};
    app::TimelineWidget* timeline_{nullptr};
    app::EffectControls* effects_{nullptr};
    app::ScopesPanel* scopes_{nullptr};
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
            std::int64_t darkFrame = 0;
            double darkness = 1e9;
            for (std::int64_t frame = 0; frame < 40; ++frame) {
                window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
                QApplication::processEvents();
                const double gray = meanGray(window.monitor()->grabFramebuffer());
                if (gray < darkness) {
                    darkness = gray;
                    darkFrame = frame;
                }
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
