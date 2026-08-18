#include "zaro/core/playback/Transport.h"

#include <algorithm>

#include "zaro/core/Check.h"

namespace zaro::playback {

Transport::Transport(Config config) : config_{std::move(config)} {
    ZARO_CHECK(!config_.ladder.empty(), "the shuttle ladder cannot be empty");
}

void Transport::shuttle(bool forward) {
    const bool alreadyGoingThatWay =
        state_ == TransportState::Playing && (forward ? speed_.isPositive() : speed_.isNegative());
    if (alreadyGoingThatWay) {
        // Climb, but stop at the top rather than wrapping back to 1x, which
        // would turn a fourth press into a surprise.
        rung_ = std::min(rung_ + 1, config_.ladder.size() - 1);
    } else {
        rung_ = 0;
    }
    const time::Rational& magnitude = config_.ladder[rung_];
    speed_ = forward ? magnitude : -magnitude;
    state_ = TransportState::Playing;
}

void Transport::pressL() {
    shuttle(true);
}
void Transport::pressJ() {
    shuttle(false);
}

void Transport::pressK() {
    state_ = TransportState::Paused;
    speed_ = time::Rational{0, 1};
    // Reset the ladder: after a pause, L should mean play, not resume 8x.
    rung_ = 0;
}

void Transport::pressSlowForward() {
    speed_ = config_.slowSpeed;
    rung_ = 0;
    state_ = TransportState::Playing;
}

void Transport::pressSlowReverse() {
    speed_ = -config_.slowSpeed;
    rung_ = 0;
    state_ = TransportState::Playing;
}

void Transport::play() {
    state_ = TransportState::Playing;
    speed_ = config_.ladder.front();
    rung_ = 0;
}

void Transport::pause() {
    state_ = TransportState::Paused;
    speed_ = time::Rational{0, 1};
    rung_ = 0;
}

void Transport::stop() {
    state_ = TransportState::Stopped;
    speed_ = time::Rational{0, 1};
    rung_ = 0;
}

void Transport::togglePlayPause() {
    if (state_ == TransportState::Playing) {
        pause();
    } else {
        play();
    }
}

}  // namespace zaro::playback
