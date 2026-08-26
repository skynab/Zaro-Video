#include <chrono>

#include "zaro/core/render/AudioGraph.h"
#include "zaro/core/render/RenderGraph.h"
#include "zaro/core/render/SmartRender.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"

#include "Ffi.h"

namespace zaro::platform::ffmpeg {

namespace {

/// The name a *decoder* would have for what this export encodes with.
///
/// Encoder names and codec names are not the same thing -- `prores_ks` writes
/// `prores` -- and the file being copied is described by the codec name. One
/// place converts, so the comparison in `smartRenderPlan` is between two names
/// of the same kind.
[[nodiscard]] std::string decoderNameFor(const EncodeSettings& settings) {
    const bool isMov =
        settings.path.size() > 4 && settings.path.compare(settings.path.size() - 4, 4, ".mov") == 0;
    const std::string encoderName =
        !settings.videoCodec.empty() ? settings.videoCodec : (isMov ? "prores_ks" : "libx264");
    if (const AVCodec* encoder = avcodec_find_encoder_by_name(encoderName.c_str())) {
        return avcodec_get_name(encoder->id);
    }
    return {};
}

}  // namespace

Status renderSequence(const model::Project& project, const RenderRequest& request,
                      const std::function<void(const RenderProgress&)>& onProgress,
                      const std::function<bool()>& keepGoing, RenderSummary* summary,
                      render::TextRasterizer* text) {
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

    // Always the originals. Delivering the small copies because somebody left a
    // toggle on is a mistake with no warning attached and no way back once the
    // file has gone out, so export takes a copy of the project with proxies off
    // rather than trusting whatever state it was handed.
    model::Project forDelivery = project;
    forDelivery.setUsingProxies(false);
    auto sourceOpened = ProjectMediaSource::open(forDelivery, request.cacheBudgetBytes);
    if (!sourceOpened) {
        return sourceOpened.error();
    }
    ProjectMediaSource& source = **sourceOpened;

    std::int64_t skippedText = 0;
    render::RenderGraph video{source};
    video.setTextRasterizer(text);
    video.setProject(&forDelivery);
    render::AudioGraph audio{source};
    audio.setProject(&forDelivery);

    EncodeSettings settings;
    settings.path = request.outputPath;
    settings.width = sequence->width();
    settings.height = sequence->height();
    settings.frameRate = rate;
    settings.audioSampleRate = audioRate;
    settings.includeAudio = request.includeAudio;
    // Passed through rather than decided here: the container's defaults are
    // still what an empty name means, so every existing caller writes the file
    // it always did.
    settings.videoCodec = request.videoCodec;
    settings.audioCodec = request.audioCodec;
    settings.videoBitRate = request.videoBitRate;
    // What the deliverable is encoded through, taken from the sequence rather
    // than from the request: the scopes and the curve editor were drawn against
    // this, and an export that used a different curve would be judged against a
    // picture nobody is going to see.
    settings.transfer = sequence->output().transfer;
    settings.highlightKnee = sequence->output().highlightKnee;

    // A copy, where the export turns out to be a piece of a file and nothing
    // has been done to it. Decided here rather than inside the encoder: what
    // makes a copy legal is a question about the edit, and the answer has to
    // be reportable whichever way it goes.
    if (request.allowCopy && (!keepGoing || keepGoing())) {
        render::SmartRenderTarget wanted;
        wanted.width = settings.width;
        wanted.height = settings.height;
        wanted.frameRate = settings.frameRate;
        wanted.videoCodec = decoderNameFor(settings);
        wanted.includeAudio = settings.includeAudio;
        wanted.audioSampleRate = settings.audioSampleRate;
        wanted.audioChannels = settings.audioChannels;

        const render::SmartRenderPlan plan =
            render::smartRenderPlan(forDelivery, *sequence, request.startFrame, frameCount, wanted);
        if (summary != nullptr) {
            summary->copyReason = plan.reason;
        }
        if (plan.possible) {
            const model::MediaRef* media = forDelivery.findMedia(plan.media);
            if (media != nullptr) {
                const auto began = std::chrono::steady_clock::now();
                auto copied = copyRange(
                    media->path, request.outputPath, plan.sourceStart, plan.frames, plan.copyAudio,
                    [&](std::int64_t done) {
                        if (onProgress) {
                            const double elapsed = std::chrono::duration<double>(
                                                       std::chrono::steady_clock::now() - began)
                                                       .count();
                            onProgress(RenderProgress{done, frameCount, elapsed});
                        }
                    },
                    keepGoing);
                if (!copied && copied.error().code() == ErrorCode::Cancelled) {
                    // Abandoning is not a reason to fall through and re-encode
                    // the very thing somebody just cancelled.
                    return copied.error();
                }
                if (copied) {
                    if (summary != nullptr) {
                        summary->copied = true;
                        summary->framesEncoded = plan.frames;
                        summary->videoPacketsWritten = copied->videoPackets;
                    }
                    return {};
                }
                // Something about the file itself said no -- most often a cut
                // that is not on a keyframe. Falling through re-encodes, which
                // always works, and the reason is reported either way.
                if (summary != nullptr) {
                    summary->copyReason = copied.error().message();
                }
            }
        }
    }

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
        skippedText += video.lastSkippedTextCount();
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
        summary->textLayersSkipped = skippedText;
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
