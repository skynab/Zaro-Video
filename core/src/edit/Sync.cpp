#include "zaro/core/edit/Sync.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "zaro/core/media/AudioAlign.h"
#include "zaro/core/media/AudioBuffer.h"
#include "zaro/core/time/Timecode.h"

namespace zaro::edit {

namespace {

Status checkAngles(const model::Clip& clip) {
    if (clip.angles.size() < 2) {
        return Error{ErrorCode::InvalidData, "syncing needs at least two angles"};
    }
    return {};
}

/// Where a file's first frame sits on the timecode clock.
std::optional<time::RationalTime> startOf(const model::MediaRef& media) {
    const media::VideoStreamInfo* video = media.info.primaryVideo();
    if (video == nullptr || !video->startTimecode.has_value()) {
        return std::nullopt;
    }
    return time::timeFromTimecode(*video->startTimecode, video->frameRate);
}

/// One angle's audio, mixed to mono, from the start of its material.
Result<std::vector<float>> monoOf(render::AudioSource& audio, model::MediaRefId media,
                                  std::int64_t sampleCount, const time::Rational& sampleRate) {
    media::AudioBuffer buffer;
    if (Status read =
            audio.read(media, time::RationalTime{0, sampleRate}, sampleCount, sampleRate, buffer);
        !read) {
        return read.error();
    }
    if (!buffer.isValid() || buffer.sampleCount() <= 0) {
        return Error{ErrorCode::InvalidData, "that angle has no audio to listen to"};
    }

    std::vector<float> mono(static_cast<std::size_t>(buffer.sampleCount()), 0.0F);
    const std::int32_t channels = buffer.channelCount();
    for (std::int32_t channel = 0; channel < channels; ++channel) {
        const float* samples = buffer.channel(channel);
        for (std::int64_t i = 0; i < buffer.sampleCount(); ++i) {
            mono[static_cast<std::size_t>(i)] += samples[i];
        }
    }
    const auto scale = 1.0F / static_cast<float>(std::max(1, channels));
    for (float& sample : mono) {
        sample *= scale;
    }
    return mono;
}

}  // namespace

Result<std::vector<AngleSync>> syncByTimecode(const model::Project& project,
                                              const model::Clip& clip) {
    if (Status ok = checkAngles(clip); !ok) {
        return ok.error();
    }

    const model::MediaRef* referenceMedia = project.findMedia(clip.angles.front().media);
    if (referenceMedia == nullptr) {
        return Error{ErrorCode::NotFound, "the first angle's media is missing"};
    }
    const std::optional<time::RationalTime> reference = startOf(*referenceMedia);
    if (!reference.has_value()) {
        return Error{ErrorCode::InvalidData,
                     "the first angle has no source timecode, so there is nothing to sync to"};
    }

    std::vector<AngleSync> out;
    out.reserve(clip.angles.size());
    for (std::size_t i = 0; i < clip.angles.size(); ++i) {
        AngleSync entry;
        entry.angle = static_cast<std::int32_t>(i);
        const model::MediaRef* media = project.findMedia(clip.angles[i].media);
        if (media == nullptr) {
            entry.reason = "its media is missing";
            out.push_back(entry);
            continue;
        }
        const std::optional<time::RationalTime> start = startOf(*media);
        if (!start.has_value()) {
            entry.reason = "it has no source timecode";
            out.push_back(entry);
            continue;
        }
        // The angle's material has to be read this much further in for its
        // clock to read the same as the reference's.
        entry.offset = (*reference - *start).rescaledTo(reference->rate());
        entry.confidence = 1.0;
        out.push_back(entry);
    }
    return out;
}

Result<std::vector<AngleSync>> syncByAudio(const model::Project& project, const model::Clip& clip,
                                           render::AudioSource& audio,
                                           const AudioSyncOptions& options) {
    if (Status ok = checkAngles(clip); !ok) {
        return ok.error();
    }
    if (options.sampleRate.num() <= 0 || options.window.frames() <= 0) {
        return Error{ErrorCode::InvalidData, "the sync window has to be a length of time"};
    }

    const time::Rational& rate = options.sampleRate;
    const std::int64_t windowSamples = options.window.rescaledTo(rate).frames();
    const std::int64_t maxLagSamples = options.maxOffset.rescaledTo(rate).frames();
    // Ten milliseconds a block: a quarter of a frame at 24fps, so the coarse
    // pass lands within a frame of the answer and the refinement never has far
    // to look.
    const std::int64_t blockSamples = std::max<std::int64_t>(1, rate.num() / (rate.den() * 100));

    auto referenceAudio = monoOf(audio, clip.angles.front().media, windowSamples, rate);
    if (!referenceAudio) {
        return referenceAudio.error();
    }

    media::AlignOptions alignOptions;
    alignOptions.maxLagSamples = maxLagSamples;
    alignOptions.blockSamples = blockSamples;
    // A second of samples around the loudest moment. Long enough to be about
    // the sound rather than about one transient, short enough that correlating
    // it at every lag in a block is not a wait.
    alignOptions.refineWindowSamples = rate.num() / rate.den();

    std::vector<AngleSync> out;
    out.reserve(clip.angles.size());
    for (std::size_t i = 0; i < clip.angles.size(); ++i) {
        AngleSync entry;
        entry.angle = static_cast<std::int32_t>(i);
        if (i == 0) {
            // The reference is where the others are measured from, so its own
            // offset is zero by definition rather than by measurement.
            entry.offset = time::RationalTime{0, rate};
            entry.confidence = 1.0;
            out.push_back(entry);
            continue;
        }

        if (project.findMedia(clip.angles[i].media) == nullptr) {
            entry.reason = "its media is missing";
            out.push_back(entry);
            continue;
        }
        auto other = monoOf(audio, clip.angles[i].media, windowSamples, rate);
        if (!other) {
            entry.reason = other.error().message();
            out.push_back(entry);
            continue;
        }

        const media::Alignment found =
            media::align(referenceAudio->data(), static_cast<std::int64_t>(referenceAudio->size()),
                         other->data(), static_cast<std::int64_t>(other->size()), alignOptions);
        entry.confidence = found.confidence;
        if (!found.reason.empty()) {
            entry.reason = std::string{found.reason};
            out.push_back(entry);
            continue;
        }
        if (found.confidence < options.minimumConfidence) {
            // The number is reported anyway: somebody looking at "0.31" learns
            // that it tried and found nothing, which is different from it not
            // having tried.
            entry.reason = "the two recordings do not sound alike enough to be sure";
            out.push_back(entry);
            continue;
        }
        entry.offset = time::RationalTime{found.offsetSamples, rate};
        out.push_back(entry);
    }
    return out;
}

}  // namespace zaro::edit
