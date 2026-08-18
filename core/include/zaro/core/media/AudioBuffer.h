#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "zaro/core/time/RationalTime.h"

namespace zaro::media {

/// Decoded audio in the pipeline's one canonical format: 32-bit float, planar,
/// deinterleaved, one vector per channel.
///
/// Everything is converted to this at the decode boundary. Mixing, gain and
/// effects then have exactly one layout to handle instead of the dozen that
/// arrive from real files, and float removes the clipping and requantisation
/// that integer intermediates would introduce at every summing stage.
class AudioBuffer {
public:
    AudioBuffer() = default;
    AudioBuffer(std::int32_t channelCount, std::int64_t sampleCount, time::Rational sampleRate);

    [[nodiscard]] bool isValid() const noexcept { return !channels_.empty(); }

    [[nodiscard]] std::int32_t channelCount() const noexcept {
        return static_cast<std::int32_t>(channels_.size());
    }
    /// Samples per channel, not samples in total.
    [[nodiscard]] std::int64_t sampleCount() const noexcept { return sampleCount_; }
    [[nodiscard]] const time::Rational& sampleRate() const noexcept { return sampleRate_; }

    [[nodiscard]] float* channel(std::int32_t index) {
        return channels_[static_cast<std::size_t>(index)].data();
    }
    [[nodiscard]] const float* channel(std::int32_t index) const {
        return channels_[static_cast<std::size_t>(index)].data();
    }

    /// Timestamp of the first sample.
    [[nodiscard]] const time::RationalTime& pts() const noexcept { return pts_; }
    void setPts(time::RationalTime value) { pts_ = std::move(value); }

    /// Duration as an exact time, for lining up against picture.
    [[nodiscard]] time::RationalTime duration() const { return {sampleCount_, sampleRate_}; }

    void resize(std::int64_t sampleCount);

    /// Peak absolute sample in a channel. Cheap, and the basis of meters.
    [[nodiscard]] float peak(std::int32_t channelIndex) const;

private:
    std::vector<std::vector<float>> channels_;
    std::int64_t sampleCount_{0};
    time::Rational sampleRate_{48000, 1};
    time::RationalTime pts_{};
};

}  // namespace zaro::media
