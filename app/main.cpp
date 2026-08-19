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
#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
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
#include "zaro/core/time/Timecode.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"
#include "zaro/platform/sdl/AudioSink.h"

#include "EffectControls.h"
#include "ProgramMonitor.h"
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
        effects_->setMinimumWidth(250);
        effects_->setMaximumWidth(330);

        // Monitor and parameters side by side, transport under them, timeline
        // across the bottom.
        auto* topRow = new QHBoxLayout;
        topRow->addWidget(monitor_, 1);
        topRow->addWidget(effects_);

        auto* layout = new QVBoxLayout(this);
        layout->addLayout(topRow, 3);
        layout->addLayout(transportRow);
        layout->addWidget(timeline_, 2);

        connect(timeline_, &app::TimelineWidget::selectionChanged, effects_,
                &app::EffectControls::setSelection);
        connect(effects_, &app::EffectControls::edited, this, [this] {
            // A parameter change alters the picture at the current playhead.
            monitor_->update();
            timeline_->update();
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

        setWindowTitle("Zaro — Program Monitor");
        refresh();
    }

    [[nodiscard]] app::ProgramMonitor* monitor() const { return monitor_; }
    [[nodiscard]] app::TimelineWidget* timeline() const { return timeline_; }
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
        refresh();
    }

    void step(std::int64_t frames) {
        stop();
        setPosition(position_ + time::RationalTime{frames, sequence_->frameRate()});
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        // Premiere's bindings, which is what hands already know.
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
            default:
                QWidget::keyPressEvent(event);
        }
    }

    void closeEvent(QCloseEvent* event) override {
        stop();
        if (waveformThread_.joinable()) {
            waveformThread_.join();
        }
        QWidget::closeEvent(event);
    }

private:
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
            for (const auto& [id, path] : wanted) {
                auto built = store.get(path);
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
