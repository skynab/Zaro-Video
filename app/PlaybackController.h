// The transport: the audio clock, the thread that feeds it, and where that
// puts the playhead.
//
// ADR-006 records why the clock is the audio device rather than a timer: a
// timer drifts against the sound card, and picture that drifts against sound is
// the one artefact an editor cannot ship. Everything here follows from that --
// the position is a rational function of how many samples the device has
// actually consumed, and the thread that fills the ring is separate so a slow
// frame cannot starve it.
//
// It was eleven members and six methods on the window, which is why the window
// had two threads in it. What the window actually needs from playback is three
// things: start, stop, and being told where the clock has got to.
#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "zaro/core/model/Sequence.h"
#include "zaro/core/playback/Transport.h"
#include "zaro/core/render/AudioGraph.h"
#include "zaro/core/time/RationalTime.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"
#include "zaro/platform/sdl/AudioSink.h"

namespace zaro::app {

class PlaybackController : public QObject {
    Q_OBJECT

public:
    explicit PlaybackController(QObject* parent = nullptr);
    ~PlaybackController() override;

    PlaybackController(const PlaybackController&) = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;
    PlaybackController(PlaybackController&&) = delete;
    PlaybackController& operator=(PlaybackController&&) = delete;

    /// What to play. Told again whenever the sequence or the media changes.
    ///
    /// Held as a pointer that the caller keeps good, the same bargain the
    /// monitor takes: a sequence looked up per audio block would be a lookup
    /// per two milliseconds on the thread that must not be late.
    void setSource(const model::Sequence* sequence, platform::ffmpeg::ProjectMediaSource* media);

    [[nodiscard]] playback::Transport& transport() noexcept { return transport_; }
    [[nodiscard]] const playback::Transport& transport() const noexcept { return transport_; }
    [[nodiscard]] bool isPlaying() const noexcept { return playing_; }

    /// Whether there is an audio device to keep time by.
    ///
    /// ADR-006: the clock is the device. Without one the transport still runs
    /// and still says it is playing, but nothing advances -- there is no
    /// elapsed-sample count to be a function of. Worth being able to ask,
    /// because "the playhead did not move" means something different on a
    /// machine with no sound card than it does on one with.
    [[nodiscard]] bool hasAudioClock() const noexcept { return sink_ != nullptr; }

    /// Whether opening a device has been tried and failed.
    ///
    /// Distinct from `!hasAudioClock()`, which is also true before anything has
    /// been tried -- the device is opened lazily on the first play. Only this
    /// one means "this machine will not give us a clock", which is the state
    /// worth telling somebody about: without a device the transport still says
    /// it is playing and the playhead never moves, and that looks like a bug
    /// rather than a missing sound card.
    [[nodiscard]] bool audioDeviceMissing() const noexcept {
        return audioAttempted_ && sink_ == nullptr;
    }
    /// Why the device would not open, in the words the platform used.
    [[nodiscard]] const QString& audioDeviceError() const noexcept { return audioError_; }

    /// Play or pause, from wherever the playhead is now.
    void togglePlay(const time::RationalTime& from);

    /// Start, or pick up a speed that has changed.
    ///
    /// Re-anchors rather than restarting, so changing shuttle speed mid-play
    /// does not make the playhead jump.
    void startIfPlaying(const time::RationalTime& from);

    void stop();

    /// Stop and join, for a window that is going away.
    void shutDown();

    /// The meters from the thread that is actually producing the audio.
    [[nodiscard]] render::AudioGraph::Meters meters() const;

signals:
    /// The clock has moved the playhead.
    void moved(const zaro::time::RationalTime& position);
    /// Started or stopped, so whatever shows that can catch up.
    void playingChanged(bool playing);

private:
    void startClock(const time::RationalTime& from);
    /// Republish the anchor for the pump thread. Call after every write to
    /// `anchorPosition_` or `anchorClock_`.
    void publishAnchor();
    void pumpAudio();
    void followClock();

    const model::Sequence* sequence_{nullptr};
    platform::ffmpeg::ProjectMediaSource* media_{nullptr};

    playback::Transport transport_;
    std::unique_ptr<platform::sdl::AudioSink> sink_;
    QTimer* clockTimer_{nullptr};

    /// Where the playhead was when the clock was last anchored, and what the
    /// device's frame counter said at that moment. Position is the difference
    /// between them, which is why both are needed and why they are set
    /// together.
    time::RationalTime anchorPosition_{};
    std::int64_t anchorClock_{0};
    std::int64_t audioWritten_{0};
    bool playing_{false};

    /// The same anchor the UI thread keeps above, in audio samples, for the
    /// pump thread to read.
    ///
    /// The pump cannot read `anchorPosition_` and `anchorClock_` directly:
    /// changing shuttle speed rewrites both from the UI thread while the pump
    /// is midway through using them, and a block mixed from half of one anchor
    /// and half of the next is audibly from the wrong place. Two atomics, both
    /// published after the values they describe, are read as a pair that is at
    /// worst one block stale rather than incoherent.
    std::atomic<std::int64_t> anchorAudioFrames_{0};
    std::atomic<std::int64_t> anchorClockFrames_{0};
    /// Whether the transport is at 1x, for the pump. Reading the transport's
    /// Rational across threads would be a race on a two-field value.
    std::atomic<bool> atUnitySpeed_{true};

    /// Whether a device has ever been asked for. See audioDeviceMissing().
    bool audioAttempted_{false};
    QString audioError_;

    std::thread audioThread_;
    std::atomic<bool> audioRunning_{false};

    mutable std::mutex meterMutex_;
    render::AudioGraph::Meters latestMeters_;
};

}  // namespace zaro::app
