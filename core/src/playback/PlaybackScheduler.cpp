#include "zaro/core/playback/PlaybackScheduler.h"

#include <algorithm>
#include <cstdlib>

#include "zaro/core/Check.h"

namespace zaro::playback {

PlaybackScheduler::PlaybackScheduler(Config config) : config_{std::move(config)} {
    ZARO_CHECK(config_.frameRate.isPositive(), "playback needs a positive frame rate");
    ZARO_CHECK(config_.audioRate.isPositive(), "playback needs a positive audio rate");
    ZARO_CHECK(config_.queueCapacity > 0, "playback needs room for at least one frame");
    anchorPosition_ = time::RationalTime{0, config_.frameRate};
    renderHead_ = anchorPosition_;
    lastShown_ = anchorPosition_;
}

time::RationalTime PlaybackScheduler::step() const {
    return time::RationalTime{goingForward() ? 1 : -1, config_.frameRate};
}

bool PlaybackScheduler::isDue(const time::RationalTime& frame,
                              const time::RationalTime& position) const {
    return goingForward() ? frame <= position : frame >= position;
}

bool PlaybackScheduler::pastEnd(const time::RationalTime& at) const {
    if (goingForward()) {
        return !config_.duration.isZero() && at >= config_.duration;
    }
    return at.frames() < 0;
}

void PlaybackScheduler::start(const time::RationalTime& at, const time::Rational& speed,
                              std::int64_t clockFrames) {
    queue_.clear();
    anchorPosition_ = at.rescaledTo(config_.frameRate);
    anchorClockFrames_ = clockFrames;
    speed_ = speed;
    renderHead_ = anchorPosition_;
    lastShown_ = anchorPosition_;
    hasShown_ = false;
    running_ = true;
}

void PlaybackScheduler::setSpeed(const time::Rational& speed, std::int64_t clockFrames) {
    // Re-anchor first, so the position is continuous across the change rather
    // than jumping by however far the old speed would have carried it.
    anchorPosition_ = positionAt(clockFrames);
    anchorClockFrames_ = clockFrames;
    const bool reversed = speed.isNegative() != speed_.isNegative();
    speed_ = speed;
    if (reversed) {
        // Everything queued is ahead in the old direction, which is behind in
        // the new one.
        queue_.clear();
        renderHead_ = anchorPosition_;
    }
}

void PlaybackScheduler::seek(const time::RationalTime& to, std::int64_t clockFrames) {
    queue_.clear();
    anchorPosition_ = to.rescaledTo(config_.frameRate);
    anchorClockFrames_ = clockFrames;
    renderHead_ = anchorPosition_;
    lastShown_ = anchorPosition_;
    hasShown_ = false;
}

void PlaybackScheduler::stop() {
    running_ = false;
    queue_.clear();
    speed_ = time::Rational{0, 1};
}

time::RationalTime PlaybackScheduler::positionAt(std::int64_t clockFrames) const {
    if (speed_.isZero()) {
        return anchorPosition_;
    }
    // Elapsed audio, converted to timeline time and scaled by speed. Exact at
    // every sample: the only floating point anywhere near this is none.
    const time::Rational elapsedSeconds =
        time::Rational{clockFrames - anchorClockFrames_, 1} / config_.audioRate;
    const time::Rational advancedFrames = elapsedSeconds * speed_ * config_.frameRate;

    // Floor, not round. The playhead is *inside* a frame for that frame's whole
    // duration; rounding to nearest would report the next frame from halfway
    // through the current one, and every frame would be presented half a frame
    // early. Flooring is also right in reverse, where it still names the frame
    // the playhead is within.
    return anchorPosition_ + time::RationalTime{advancedFrames.floorToInt(), config_.frameRate};
}

std::optional<time::RationalTime> PlaybackScheduler::nextRenderTarget() const {
    if (!running_ || speed_.isZero()) {
        return std::nullopt;
    }
    if (queue_.size() >= config_.queueCapacity) {
        return std::nullopt;
    }
    if (pastEnd(renderHead_)) {
        return std::nullopt;
    }
    return renderHead_;
}

void PlaybackScheduler::submit(const time::RationalTime& at, render::RgbaImage image) {
    ++stats_.rendered;
    // Only accept frames at or ahead of the render head. A frame that arrives
    // for a time already passed is work the catch-up policy decided to skip,
    // and queueing it would undo that decision.
    if (goingForward() ? at < renderHead_ : at > renderHead_) {
        ++stats_.submittedTooLate;
        return;
    }
    queue_.push_back(Slot{at, std::move(image)});
    renderHead_ = at + step();
}

PresentResult PlaybackScheduler::present(std::int64_t clockFrames) {
    if (!running_) {
        return PresentResult{};
    }

    const time::RationalTime position = positionAt(clockFrames);

    if (config_.loop && !config_.duration.isZero() && pastEnd(position)) {
        seek(time::RationalTime{0, config_.frameRate}, clockFrames);
        return PresentResult{PresentAction::Repeat, lastShown_, nullptr, 0};
    }

    // Take everything at or behind the playhead. The newest of them is what
    // goes on screen; the rest are late and are discarded rather than shown out
    // of time. The chosen frame is kept in `current_` so the pointer handed
    // back stays valid until the next call.
    std::int32_t popped = 0;
    while (!queue_.empty() && isDue(queue_.front().at, position)) {
        current_ = std::move(queue_.front());
        queue_.pop_front();
        ++popped;
    }

    PresentResult result;
    const std::int32_t dropped = popped > 0 ? popped - 1 : 0;

    if (popped > 0) {
        result.action = PresentAction::Present;
        result.shown = current_.at;
        result.image = &current_.image;
        lastShown_ = current_.at;
        hasShown_ = true;
        ++stats_.presented;
    } else if (!queue_.empty()) {
        result.action = PresentAction::Repeat;
        result.shown = lastShown_;
        result.image = hasShown_ ? &current_.image : nullptr;
        ++stats_.repeated;
    } else {
        result.action = PresentAction::Starve;
        result.shown = lastShown_;
        result.image = hasShown_ ? &current_.image : nullptr;
        ++stats_.starved;
    }

    result.dropped = dropped;
    stats_.dropped += dropped;

    if (hasShown_) {
        const std::int64_t offset =
            std::abs((position - lastShown_).rescaledTo(config_.frameRate).frames());
        stats_.worstOffsetFrames = std::max(stats_.worstOffsetFrames, offset);
    }

    // Do not render what cannot be shown. A renderer that falls behind and then
    // works through its backlog in order never catches up.
    if (dropped > 0 || result.action == PresentAction::Starve) {
        const time::RationalTime catchUp = position + step();
        if (goingForward() ? catchUp > renderHead_ : catchUp < renderHead_) {
            renderHead_ = catchUp;
        }
    }
    return result;
}

}  // namespace zaro::playback
