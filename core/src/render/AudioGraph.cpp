#include "zaro/core/render/AudioGraph.h"

#include <algorithm>
#include <cmath>

namespace zaro::render {

float gainFromDb(double decibels) noexcept {
    if (decibels <= -100.0) {
        return 0.0F;
    }
    return static_cast<float>(std::pow(10.0, decibels / 20.0));
}

void panGains(double pan, float& leftGain, float& rightGain) noexcept {
    const double clamped = std::clamp(pan, -1.0, 1.0);
    // Constant power: the angle sweeps a quarter turn from hard left to hard
    // right, and sin^2 + cos^2 = 1 keeps total power flat across the sweep.
    const double angle = (clamped + 1.0) * 0.25 * std::acos(-1.0);
    leftGain = static_cast<float>(std::cos(angle));
    rightGain = static_cast<float>(std::sin(angle));
}

void balanceGains(double pan, float& leftGain, float& rightGain) noexcept {
    const double clamped = std::clamp(pan, -1.0, 1.0);
    leftGain = static_cast<float>(clamped <= 0.0 ? 1.0 : 1.0 - clamped);
    rightGain = static_cast<float>(clamped >= 0.0 ? 1.0 : 1.0 + clamped);
}

Result<media::AudioBuffer> AudioGraph::mix(const model::Sequence& sequence,
                                           const time::RationalTime& start,
                                           std::int64_t sampleCount, std::int32_t channelCount) {
    if (sampleCount < 0) {
        return Error{ErrorCode::InvalidData, "cannot mix a negative number of samples"};
    }
    if (channelCount < 1 || channelCount > 2) {
        return Error{ErrorCode::Unsupported, "the mixer currently handles mono and stereo only"};
    }

    const time::Rational& rate = sequence.audioSampleRate();
    media::AudioBuffer mixed{channelCount, sampleCount, rate};
    lastClipCount_ = 0;
    if (sampleCount == 0) {
        return mixed;
    }

    // The whole mix is addressed in samples, from here down.
    const time::RationalTime blockStart = start.rescaledTo(rate);
    const time::TimeRange block{blockStart, time::RationalTime{sampleCount, rate}};

    media::AudioBuffer scratch;

    for (const model::Track& track : sequence.audioTracks()) {
        if (track.isMuted()) {
            continue;
        }
        const float trackGain = gainFromDb(track.gainDb());
        // The track bus is already stereo, so its pan control is a balance.
        float trackLeft = 1.0F;
        float trackRight = 1.0F;
        balanceGains(track.pan(), trackLeft, trackRight);

        for (const model::Clip& clip : track.clips()) {
            if (!clip.enabled) {
                continue;
            }
            const time::TimeRange clipRange = clip.timelineRange.rescaledTo(rate);
            const auto overlap = clipRange.intersection(block);
            if (!overlap) {
                continue;
            }

            const std::int64_t offsetInBlock = overlap->start().frames() - blockStart.frames();
            const std::int64_t wanted = overlap->duration().frames();
            if (wanted <= 0) {
                continue;
            }

            const time::RationalTime sourceStart = clip.sourceTimeAt(overlap->start());
            if (Status status = source_->read(clip.source, sourceStart, wanted, rate, scratch);
                !status) {
                // A clip whose audio cannot be read is silence, not a failed
                // render. The same reasoning as a missing picture: a hole is
                // diagnosable, a stalled export is not.
                continue;
            }

            const std::int64_t available = std::min<std::int64_t>(wanted, scratch.sampleCount());
            const std::int32_t sourceChannels = scratch.channelCount();

            const float clipGain = gainFromDb(clip.gainDb);
            // A mono clip is being *placed* in the stereo field, so it gets the
            // constant-power law. A stereo clip is already placed, so its
            // control is a balance and leaves a centred clip untouched.
            float clipLeft = 1.0F;
            float clipRight = 1.0F;
            if (sourceChannels == 1) {
                panGains(clip.pan, clipLeft, clipRight);
            } else {
                balanceGains(clip.pan, clipLeft, clipRight);
            }

            for (std::int32_t channel = 0; channel < channelCount; ++channel) {
                // Mono sources feed both outputs; a stereo source keeps its
                // sides. Anything wider is folded by taking the first channels,
                // which is a placeholder until real channel mapping arrives.
                const std::int32_t from = std::min(channel, sourceChannels - 1);
                const float pan = channelCount == 1 ? 1.0F
                                                    : (channel == 0 ? clipLeft * trackLeft
                                                                    : clipRight * trackRight);
                const float gain = clipGain * trackGain * pan;

                const float* in = scratch.channel(from);
                float* out = mixed.channel(channel);
                for (std::int64_t i = 0; i < available; ++i) {
                    out[offsetInBlock + i] += in[i] * gain;
                }
            }
            ++lastClipCount_;
        }
    }
    return mixed;
}

}  // namespace zaro::render
