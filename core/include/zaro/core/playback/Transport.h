#pragma once

#include <vector>

#include "zaro/core/time/Rational.h"

namespace zaro::playback {

enum class TransportState { Stopped, Playing, Paused };

/// The JKL shuttle, and the play/pause state around it.
///
/// Speed is rational rather than a double because it multiplies into the
/// mapping from elapsed audio samples to timeline position. A speed of 1/3 as
/// a double would put the playhead a fraction of a frame out immediately and
/// visibly out after a minute; as a rational it is exact at every sample.
class Transport {
public:
    struct Config {
        /// Successive presses of J or L climb this ladder. Four rungs matches
        /// what editors reach for; the ladder is configurable because shuttle
        /// speeds are a matter of taste and muscle memory.
        std::vector<time::Rational> ladder{time::Rational::fromInt(1), time::Rational::fromInt(2),
                                           time::Rational::fromInt(4), time::Rational::fromInt(8)};
        /// Reached by holding K with J or L: frame-accurate crawl.
        time::Rational slowSpeed{1, 4};
    };

    Transport() : Transport{Config{}} {}
    explicit Transport(Config config);

    /// Forward. From stopped or reverse it starts at the first rung; from
    /// forward it climbs.
    void pressL();
    /// Reverse, mirroring L.
    void pressJ();
    /// Pause, and reset the shuttle so the next L starts at 1x rather than
    /// resuming a fast shuttle nobody asked for.
    void pressK();

    /// Held-K modifiers, the slow-shuttle pair.
    void pressSlowForward();
    void pressSlowReverse();

    void play();
    void pause();
    void stop();
    void togglePlayPause();

    [[nodiscard]] TransportState state() const noexcept { return state_; }
    [[nodiscard]] bool isPlaying() const noexcept { return state_ == TransportState::Playing; }

    /// Zero when not playing; negative when running in reverse.
    [[nodiscard]] const time::Rational& speed() const noexcept { return speed_; }

    /// Which rung of the ladder is in use, for a shuttle indicator.
    [[nodiscard]] std::size_t rung() const noexcept { return rung_; }

private:
    void shuttle(bool forward);

    Config config_;
    TransportState state_{TransportState::Stopped};
    time::Rational speed_{0, 1};
    std::size_t rung_{0};
};

}  // namespace zaro::playback
