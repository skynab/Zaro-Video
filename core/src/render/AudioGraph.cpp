#include "zaro/core/render/AudioGraph.h"

#include <algorithm>
#include <cmath>
#include <vector>

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
    meters_.tracks.clear();
    meters_.reduction.clear();
    meters_.master.assign(static_cast<std::size_t>(channelCount), 0.0F);
    if (sampleCount == 0) {
        return mixed;
    }

    // The whole mix is addressed in samples, from here down.
    const time::RationalTime blockStart = start.rescaledTo(rate);
    const time::TimeRange block{blockStart, time::RationalTime{sampleCount, rate}};

    media::AudioBuffer scratch;

    for (const model::Track& track : sequence.audioTracks()) {
        if (!sequence.isAudible(track)) {
            continue;
        }
        float trackPeak = 0.0F;
        const float trackGain = gainFromDb(track.gainDb());

        // The track's own bus. Clips sum into this, the processing chain runs
        // on it, and only then does the fader apply -- which is where an insert
        // goes on every console ever built, and means pulling the fader down
        // turns down what the compressor did rather than changing what it does.
        media::AudioBuffer bus{channelCount, sampleCount, rate};
        // The track bus is already stereo, so its pan control is a balance.
        float trackLeft = 1.0F;
        float trackRight = 1.0F;
        balanceGains(track.pan(), trackLeft, trackRight);

        // Reused across clips so an automated mix does not allocate per block.
        // Sized to the block, never to the whole clip.
        std::vector<float> resampled;
        std::vector<float> clipGains;
        std::vector<float> leftPan;
        std::vector<float> rightPan;

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
            // A retimed clip covers more (or less) source than it occupies on
            // the timeline, so it has to read that much and resample. Without
            // this the picture retimes and the sound does not, which is drift
            // that grows for as long as the clip lasts.
            const double speed = clip.speed();
            const bool retimed = clip.reversed || std::fabs(speed - 1.0) > 1e-9;
            const std::int64_t toRead =
                retimed ? std::max<std::int64_t>(
                              2, static_cast<std::int64_t>(std::ceil(wanted * speed)) + 2)
                        : wanted;
            const time::RationalTime readFrom =
                clip.reversed
                    ? clip.sourceTimeAt(overlap->endExclusive() - time::RationalTime{1, rate})
                    : sourceStart;
            if (Status status = source_->read(clip.source, readFrom, toRead, rate, scratch);
                !status) {
                // A clip whose audio cannot be read is silence, not a failed
                // render. The same reasoning as a missing picture: a hole is
                // diagnosable, a stalled export is not.
                continue;
            }

            if (retimed) {
                // Linear interpolation, and the pitch moves with the speed --
                // which is what a plain speed change does everywhere. Holding
                // pitch is a different feature with a different name, and
                // pretending this one does it would be worse than not having
                // it.
                resampled.resize(static_cast<std::size_t>(wanted) *
                                 static_cast<std::size_t>(scratch.channelCount()));
                const std::int64_t haveSamples = scratch.sampleCount();
                for (std::int32_t channel = 0; channel < scratch.channelCount(); ++channel) {
                    const float* in = scratch.channel(channel);
                    for (std::int64_t i = 0; i < wanted; ++i) {
                        const double position = clip.reversed
                                                    ? (static_cast<double>(wanted - 1 - i) * speed)
                                                    : (static_cast<double>(i) * speed);
                        const auto low = static_cast<std::int64_t>(position);
                        const std::int64_t high = std::min(low + 1, haveSamples - 1);
                        const auto fraction =
                            static_cast<float>(position - static_cast<double>(low));
                        const float a = low < haveSamples && low >= 0 ? in[low] : 0.0F;
                        const float b = high >= 0 && high < haveSamples ? in[high] : 0.0F;
                        resampled[(static_cast<std::size_t>(channel) *
                                   static_cast<std::size_t>(wanted)) +
                                  static_cast<std::size_t>(i)] = a + ((b - a) * fraction);
                    }
                }
                media::AudioBuffer retimedBuffer{scratch.channelCount(), wanted, rate};
                for (std::int32_t channel = 0; channel < scratch.channelCount(); ++channel) {
                    std::copy_n(resampled.data() + (static_cast<std::size_t>(channel) *
                                                    static_cast<std::size_t>(wanted)),
                                static_cast<std::size_t>(wanted), retimedBuffer.channel(channel));
                }
                scratch = std::move(retimedBuffer);
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

            // Automation is evaluated per sample rather than per block. A gain
            // held constant across a block steps at the block boundary, and
            // that boundary is a property of the audio device's buffer size,
            // not of the edit: the same project would zipper differently on
            // different hardware, and at 512 samples a fade becomes a staircase
            // of audible clicks.
            //
            // Once per sample, not once per sample per channel: the curve does
            // not know which speaker it is feeding.
            const bool automated = clip.animation.find(model::Param::GainDb) != nullptr ||
                                   clip.animation.find(model::Param::Pan) != nullptr;
            if (automated) {
                clipGains.resize(static_cast<std::size_t>(available));
                leftPan.resize(static_cast<std::size_t>(available));
                rightPan.resize(static_cast<std::size_t>(available));
                for (std::int64_t i = 0; i < available; ++i) {
                    const time::RationalTime when{overlap->start().frames() + i, rate};
                    clipGains[static_cast<std::size_t>(i)] = gainFromDb(clip.gainDbAt(when));
                    float left = 1.0F;
                    float right = 1.0F;
                    if (sourceChannels == 1) {
                        panGains(clip.panAt(when), left, right);
                    } else {
                        balanceGains(clip.panAt(when), left, right);
                    }
                    leftPan[static_cast<std::size_t>(i)] = left;
                    rightPan[static_cast<std::size_t>(i)] = right;
                }
            }

            for (std::int32_t channel = 0; channel < channelCount; ++channel) {
                // Mono sources feed both outputs; a stereo source keeps its
                // sides. Anything wider is folded by taking the first channels,
                // which is a placeholder until real channel mapping arrives.
                const std::int32_t from = std::min(channel, sourceChannels - 1);
                // Clip gain and pan only. The track's fader and balance come
                // after the processing, below.
                const float pan = channelCount == 1 ? 1.0F : (channel == 0 ? clipLeft : clipRight);
                const float gain = clipGain * pan;

                const float* in = scratch.channel(from);
                float* out = bus.channel(channel);
                if (!automated) {
                    for (std::int64_t i = 0; i < available; ++i) {
                        out[offsetInBlock + i] += in[i] * gain;
                    }
                    continue;
                }
                const std::vector<float>& side = channel == 0 ? leftPan : rightPan;
                for (std::int64_t i = 0; i < available; ++i) {
                    const auto at = static_cast<std::size_t>(i);
                    const float placed = channelCount == 1 ? 1.0F : side[at];
                    out[offsetInBlock + i] += in[i] * clipGains[at] * placed;
                }
            }
            ++lastClipCount_;
        }

        // Processing, then the fader. The chain keeps state between blocks, so
        // it is kept per track rather than rebuilt: a filter that forgot its
        // last two samples every block would ring at every boundary.
        TrackProcessor& chain = processors_[track.id().value()];
        chain.configure(track.eq(), track.compressor(), rate.toDouble(), channelCount);
        std::vector<float*> busChannels(static_cast<std::size_t>(channelCount));
        for (std::int32_t channel = 0; channel < channelCount; ++channel) {
            busChannels[static_cast<std::size_t>(channel)] = bus.channel(channel);
        }
        chain.process(busChannels.data(), channelCount, sampleCount);
        meters_.reduction[track.id().value()] = chain.lastReductionDb();

        for (std::int32_t channel = 0; channel < channelCount; ++channel) {
            const float busGain =
                trackGain * (channelCount == 1 ? 1.0F : (channel == 0 ? trackLeft : trackRight));
            const float* in = bus.channel(channel);
            float* out = mixed.channel(channel);
            for (std::int64_t i = 0; i < sampleCount; ++i) {
                const float value = in[i] * busGain;
                // Measured post-fader: what the track contributes, which is
                // what a strip's meter is for.
                trackPeak = std::max(trackPeak, std::fabs(value));
                out[i] += value;
            }
        }
        meters_.tracks[track.id().value()] = trackPeak;
    }

    // The master, measured on the summed result: it is the only figure that has
    // to account for two tracks adding up past full scale.
    for (std::int32_t channel = 0; channel < channelCount; ++channel) {
        const float* samples = mixed.channel(channel);
        float peak = 0.0F;
        for (std::int64_t i = 0; i < sampleCount; ++i) {
            peak = std::max(peak, std::fabs(samples[i]));
        }
        meters_.master[static_cast<std::size_t>(channel)] = peak;
    }
    return mixed;
}

void AudioGraph::resetProcessing() {
    for (auto& [id, processor] : processors_) {
        processor.reset();
    }
}

}  // namespace zaro::render
