#include <chrono>

#include "zaro/core/render/AudioGraph.h"
#include "zaro/core/render/RenderGraph.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"

#include "Ffi.h"

namespace zaro::platform::ffmpeg {

Status renderSequence(const model::Project& project, const RenderRequest& request,
                      const std::function<void(const RenderProgress&)>& onProgress,
                      const std::function<bool()>& keepGoing, RenderSummary* summary) {
    const model::Sequence* sequence = project.findSequence(request.sequence);
    if (sequence == nullptr) {
        return Error{ErrorCode::NotFound, "no such sequence"};
    }

    const time::Rational& rate = sequence->frameRate();
    const time::Rational& audioRate = sequence->audioSampleRate();

    std::int64_t frameCount = request.frameCount;
    if (frameCount < 0) {
        frameCount = sequence->duration().frames() - request.startFrame;
    }
    if (frameCount <= 0) {
        return Error{ErrorCode::InvalidData, "there is nothing in that range to render"};
    }

    auto sourceOpened = ProjectMediaSource::open(project, request.cacheBudgetBytes);
    if (!sourceOpened) {
        return sourceOpened.error();
    }
    ProjectMediaSource& source = **sourceOpened;

    render::RenderGraph video{source};
    render::AudioGraph audio{source};

    EncodeSettings settings;
    settings.path = request.outputPath;
    settings.width = sequence->width();
    settings.height = sequence->height();
    settings.frameRate = rate;
    settings.audioSampleRate = audioRate;
    settings.includeAudio = request.includeAudio;

    auto encoderOpened = Encoder::open(settings);
    if (!encoderOpened) {
        return encoderOpened.error();
    }
    Encoder& encoder = **encoderOpened;

    // Audio is addressed by an exact rational relationship to the frame number
    // rather than by adding a per-frame duration to a running total. At 29.97
    // there are 1601.6 samples per frame; accumulating that rounded drifts by a
    // sample every few frames and by a visible lip-sync error over an hour.
    const time::Rational samplesPerFrame = audioRate / rate;
    const auto sampleAtFrame = [&](std::int64_t frame) {
        return (time::Rational::fromInt(frame) * samplesPerFrame).floorToInt();
    };

    render::RgbaImage frame;
    const auto began = std::chrono::steady_clock::now();

    for (std::int64_t index = 0; index < frameCount; ++index) {
        if (keepGoing && !keepGoing()) {
            return Error{ErrorCode::Cancelled, "render abandoned"};
        }

        const std::int64_t timelineFrame = request.startFrame + index;
        const time::RationalTime at{timelineFrame, rate};

        if (Status status = video.compositeInto(*sequence, at, frame); !status) {
            return status;
        }
        if (Status status = encoder.writeVideo(frame); !status) {
            return status;
        }

        if (request.includeAudio) {
            const std::int64_t from = sampleAtFrame(timelineFrame);
            const std::int64_t to = sampleAtFrame(timelineFrame + 1);
            auto mixed = audio.mix(*sequence, time::RationalTime{from, audioRate}, to - from);
            if (!mixed) {
                return mixed.error();
            }
            if (Status status = encoder.writeAudio(*mixed); !status) {
                return status;
            }
        }

        if (onProgress) {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
            onProgress(RenderProgress{index + 1, frameCount, elapsed});
        }
    }

    if (Status status = encoder.finish(); !status) {
        return status;
    }

    if (summary != nullptr) {
        summary->framesEncoded = encoder.framesWritten();
        summary->videoPacketsWritten = encoder.videoPacketsWritten();
        summary->audioSamplesWritten = encoder.samplesWritten();
        summary->audioSamplesExpected =
            request.includeAudio
                ? sampleAtFrame(request.startFrame + frameCount) - sampleAtFrame(request.startFrame)
                : 0;
        summary->cacheHits = source.cache().hits();
        summary->cacheMisses = source.cache().misses();
    }
    return {};
}

}  // namespace zaro::platform::ffmpeg
