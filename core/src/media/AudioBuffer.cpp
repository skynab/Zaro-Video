#include "zaro/core/media/AudioBuffer.h"

#include <algorithm>
#include <cmath>

#include "zaro/core/Check.h"

namespace zaro::media {

AudioBuffer::AudioBuffer(std::int32_t channelCount, std::int64_t sampleCount,
                         time::Rational sampleRate)
    : channels_(static_cast<std::size_t>(channelCount)),
      sampleCount_{sampleCount},
      sampleRate_{std::move(sampleRate)} {
    ZARO_CHECK(channelCount > 0, "AudioBuffer needs at least one channel");
    ZARO_CHECK(sampleCount >= 0, "AudioBuffer sample count must not be negative");
    ZARO_CHECK(sampleRate_.isPositive(), "AudioBuffer needs a positive sample rate");
    for (auto& channel : channels_) {
        channel.assign(static_cast<std::size_t>(sampleCount), 0.0F);
    }
}

void AudioBuffer::resize(std::int64_t sampleCount) {
    ZARO_CHECK(sampleCount >= 0, "AudioBuffer sample count must not be negative");
    for (auto& channel : channels_) {
        channel.resize(static_cast<std::size_t>(sampleCount), 0.0F);
    }
    sampleCount_ = sampleCount;
}

float AudioBuffer::peak(std::int32_t channelIndex) const {
    const auto& samples = channels_[static_cast<std::size_t>(channelIndex)];
    float maximum = 0.0F;
    for (const float sample : samples) {
        maximum = std::max(maximum, std::abs(sample));
    }
    return maximum;
}

}  // namespace zaro::media
