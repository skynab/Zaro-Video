#include "zaro/core/render/Ducking.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "zaro/core/media/AudioAlign.h"
#include "zaro/core/media/AudioBuffer.h"

namespace zaro::render {
namespace {

/// One envelope block. Ten milliseconds is short enough to catch the start of a
/// word and long enough that the envelope is a loudness rather than a waveform.
constexpr std::int64_t kBlockDivisor = 100;

/// Whether a clip is one whose sound should push others out of the way.
bool isDialogue(const model::Clip& clip) {
    return clip.enabled && clip.role == model::AudioRole::Dialogue;
}

}  // namespace

Result<model::Curve> duckingCurve(const model::Sequence& sequence, const model::Clip& ducked,
                                  AudioSource& audio, const DuckingOptions& options) {
    model::Curve curve;
    if (ducked.timelineRange.isEmpty()) {
        return Error{ErrorCode::InvalidData, "that clip has no duration"};
    }
    if (options.sampleRate.num() <= 0 || options.sampleRate.den() <= 0) {
        return Error{ErrorCode::InvalidData, "the analysis needs a real sample rate"};
    }

    const time::Rational& rate = options.sampleRate;
    const std::int64_t block = std::max<std::int64_t>(1, rate.num() / (rate.den() * kBlockDivisor));
    const time::TimeRange span = ducked.timelineRange.rescaledTo(rate);
    const std::int64_t first = span.start().frames();
    const std::int64_t total = span.duration().frames();
    const std::int64_t blocks = (total + block - 1) / block;
    if (blocks <= 0) {
        return Error{ErrorCode::InvalidData, "that clip is too short to duck"};
    }

    // True where somebody is speaking, one entry per block.
    std::vector<bool> speaking(static_cast<std::size_t>(blocks), false);
    bool foundDialogue = false;

    for (const model::Track& track : sequence.audioTracks()) {
        if (!sequence.isAudible(track)) {
            continue;
        }
        for (const model::Clip& clip : track.clips()) {
            if (!isDialogue(clip)) {
                continue;
            }
            const auto overlap = clip.timelineRange.rescaledTo(rate).intersection(span);
            if (!overlap || overlap->isEmpty()) {
                continue;
            }
            foundDialogue = true;

            const std::int64_t count = overlap->duration().frames();
            media::AudioBuffer buffer;
            if (Status read =
                    audio.read(clip.activeSource(), clip.activeBaseSourceTimeAt(overlap->start()),
                               count, rate, buffer);
                !read) {
                return read.error();
            }
            std::vector<float> mono(static_cast<std::size_t>(buffer.sampleCount()), 0.0F);
            for (std::int32_t channel = 0; channel < buffer.channelCount(); ++channel) {
                const float* samples = buffer.channel(channel);
                for (std::int64_t i = 0; i < buffer.sampleCount(); ++i) {
                    mono[static_cast<std::size_t>(i)] += samples[i];
                }
            }
            const auto scale = 1.0F / static_cast<float>(std::max(1, buffer.channelCount()));
            for (float& sample : mono) {
                sample *= scale;
            }

            const std::vector<double> levels =
                media::envelope(mono.data(), static_cast<std::int64_t>(mono.size()), block);
            const std::int64_t offset = overlap->start().frames() - first;
            for (std::size_t i = 0; i < levels.size(); ++i) {
                if (levels[i] < options.threshold) {
                    continue;
                }
                const std::int64_t at = (offset / block) + static_cast<std::int64_t>(i);
                if (at >= 0 && at < blocks) {
                    speaking[static_cast<std::size_t>(at)] = true;
                }
            }
        }
    }

    if (!foundDialogue) {
        return Error{ErrorCode::NotFound, "there is no dialogue over that clip to duck under"};
    }

    // Hold: a gap shorter than this is not a pause, it is the space between two
    // sentences, and lifting the music through it is more distracting than not
    // ducking at all.
    const std::int64_t holdBlocks =
        std::max<std::int64_t>(0, options.hold.rescaledTo(rate).frames() / block);
    std::vector<bool> held = speaking;
    for (std::int64_t i = 0; i < blocks; ++i) {
        if (!speaking[static_cast<std::size_t>(i)]) {
            continue;
        }
        const std::int64_t until = std::min(blocks, i + holdBlocks + 1);
        for (std::int64_t j = i; j < until; ++j) {
            held[static_cast<std::size_t>(j)] = true;
        }
    }

    const auto sourceAt = [&](std::int64_t blockIndex) {
        const time::RationalTime when{first + (blockIndex * block), rate};
        return ducked.baseSourceTimeAt(when.rescaledTo(sequence.frameRate()));
    };
    const auto keyframe = [&curve](const time::RationalTime& when, double db) {
        curve.set(model::Keyframe{when, db, model::Interpolation::Linear, {}, {}});
    };

    /// A keyframe strictly before another.
    ///
    /// Keyframe times are quantised to frames, so a fade shorter than one lands
    /// on the same instant as the dip it precedes -- and `Curve::set` replaces
    /// rather than appends, so the level before the dip disappears and the ramp
    /// runs all the way from the start of the clip. Nudging back one frame
    /// keeps the shape the options asked for at the resolution the model has.
    const auto before = [&](const time::RationalTime& when) {
        return when - time::RationalTime{1, when.rate()};
    };

    const std::int64_t downBlocks =
        std::max<std::int64_t>(1, options.fadeDown.rescaledTo(rate).frames() / block);
    const std::int64_t upBlocks =
        std::max<std::int64_t>(1, options.fadeUp.rescaledTo(rate).frames() / block);

    // The clip's own level either side of every dip, so ducking is a change to
    // what somebody set rather than a replacement of it.
    const double open = ducked.gainDb;
    const double closed = ducked.gainDb + options.duckDb;

    keyframe(sourceAt(0), held.front() ? closed : open);
    for (std::int64_t i = 1; i < blocks; ++i) {
        const bool was = held[static_cast<std::size_t>(i - 1)];
        const bool now = held[static_cast<std::size_t>(i)];
        if (was == now) {
            continue;
        }
        const time::RationalTime edge = sourceAt(i);
        if (now) {
            const time::RationalTime opens = sourceAt(std::max<std::int64_t>(0, i - downBlocks));
            keyframe(opens < edge ? opens : before(edge), open);
            keyframe(edge, closed);
        } else {
            const time::RationalTime closes = sourceAt(std::min(blocks - 1, i + upBlocks));
            keyframe(edge, closed);
            keyframe(closes > edge ? closes : edge + time::RationalTime{1, edge.rate()}, open);
        }
    }
    keyframe(sourceAt(blocks - 1), held.back() ? closed : open);
    return curve;
}

}  // namespace zaro::render
