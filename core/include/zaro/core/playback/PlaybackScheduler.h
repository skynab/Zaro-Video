#pragma once

#include <cstdint>
#include <deque>
#include <optional>

#include "zaro/core/render/RgbaImage.h"
#include "zaro/core/time/RationalTime.h"

namespace zaro::playback {

enum class PresentAction {
    Idle,     ///< Not running.
    Present,  ///< A new frame is due.
    Repeat,   ///< The due frame has not changed; keep showing the last one.
    Starve,   ///< Nothing rendered in time. The renderer is not keeping up.
};

struct PresentResult {
    PresentAction action{PresentAction::Idle};
    time::RationalTime shown;
    const render::RgbaImage* image{nullptr};
    /// Frames skipped because the clock had already passed them.
    std::int32_t dropped{0};
};

struct PlaybackStats {
    std::int64_t presented{0};
    std::int64_t dropped{0};
    std::int64_t repeated{0};
    std::int64_t starved{0};
    std::int64_t rendered{0};
    std::int64_t submittedTooLate{0};
    /// Worst gap between the frame on screen and where the clock says we are,
    /// in frames. This is the number that decides whether playback is in sync.
    std::int64_t worstOffsetFrames{0};
};

/// Decides which frame to show and when, with no threads and no devices.
///
/// Everything time-sensitive about playback lives here and is driven by an
/// injected clock reading, so a ten-minute playback under load can be simulated
/// deterministically in milliseconds -- including the cases that matter, where
/// the renderer cannot keep up. Playback bugs that only appear on a slow
/// machine after eight minutes are not otherwise testable, and playback is
/// where editors die.
///
/// Two policies are deliberate:
///
///  * **Video is dropped, audio never is.** Audio is the clock; a gap in it is
///    both audible and a lie about how much time has passed. A dropped frame is
///    neither.
///  * **Frames already behind the playhead are not rendered.** A renderer that
///    falls behind and then works through its backlog in order never catches
///    up. Skipping ahead is what makes playback recover instead of degrade.
class PlaybackScheduler {
public:
    struct Config {
        time::Rational frameRate{time::rates::fps25};
        time::Rational audioRate{time::rates::hz48000};
        /// How many rendered frames may wait ahead of the playhead. Too small
        /// and any hitch starves; too large and a seek throws away more work.
        std::size_t queueCapacity{6};
        /// End of the sequence. Playback stops here unless looping.
        time::RationalTime duration{};
        bool loop{false};
    };

    explicit PlaybackScheduler(Config config);

    /// Begin at `at`, with the clock currently reading `clockFrames` samples.
    void start(const time::RationalTime& at, const time::Rational& speed, std::int64_t clockFrames);
    /// Change speed without moving the playhead. Re-anchors the clock so the
    /// position stays continuous across the change.
    void setSpeed(const time::Rational& speed, std::int64_t clockFrames);
    /// Jump. Everything queued is discarded: it is all for the wrong time now.
    void seek(const time::RationalTime& to, std::int64_t clockFrames);
    void stop();

    [[nodiscard]] bool isRunning() const noexcept { return running_; }
    [[nodiscard]] const time::Rational& speed() const noexcept { return speed_; }

    /// Where the clock says the playhead is.
    [[nodiscard]] time::RationalTime positionAt(std::int64_t clockFrames) const;

    /// The frame the renderer should produce next, or nothing when the queue is
    /// full or playback has run off the end.
    [[nodiscard]] std::optional<time::RationalTime> nextRenderTarget() const;

    /// Hand back a rendered frame. Advances the render head past it.
    void submit(const time::RationalTime& at, render::RgbaImage image);

    /// Choose what to show now.
    PresentResult present(std::int64_t clockFrames);

    [[nodiscard]] const PlaybackStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t queued() const noexcept { return queue_.size(); }
    [[nodiscard]] const time::RationalTime& renderHead() const noexcept { return renderHead_; }

private:
    struct Slot {
        time::RationalTime at;
        render::RgbaImage image;
    };

    [[nodiscard]] bool goingForward() const noexcept { return !speed_.isNegative(); }
    /// Whether `frame` is at or behind the playhead, in the direction of travel.
    [[nodiscard]] bool isDue(const time::RationalTime& frame,
                             const time::RationalTime& position) const;
    [[nodiscard]] time::RationalTime step() const;
    [[nodiscard]] bool pastEnd(const time::RationalTime& at) const;

    Config config_;
    std::deque<Slot> queue_;
    PlaybackStats stats_;

    time::Rational speed_{1, 1};
    time::RationalTime anchorPosition_{};
    std::int64_t anchorClockFrames_{0};
    time::RationalTime renderHead_{};
    /// The frame currently on screen. Held so the pointer returned by
    /// present() stays valid until the next call.
    Slot current_;
    time::RationalTime lastShown_{};
    bool hasShown_{false};
    bool running_{false};
};

}  // namespace zaro::playback
