#include <algorithm>
#include <map>
#include <memory>

#include "zaro/core/media/Decoder.h"
#include "zaro/core/render/ColorPipeline.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"

#include "Ffi.h"

namespace zaro::platform::ffmpeg {

struct ProjectMediaSource::State {
    struct VideoEntry {
        std::unique_ptr<media::VideoDecoder> decoder;
    };

    /// The frame handed out by sourceFrameFor, kept alive until the next call.
    media::VideoFrame lastSourceFrame;
    struct AudioEntry {
        std::unique_ptr<media::AudioDecoder> decoder;
        media::AudioBuffer pending;     ///< Decoded but not yet handed out.
        std::int64_t pendingOffset{0};  ///< How much of `pending` is consumed.
        std::int64_t position{-1};      ///< Next sample this decoder will produce.
    };

    std::map<std::uint64_t, std::string> paths;
    std::map<std::uint64_t, VideoEntry> video;
    std::map<std::uint64_t, AudioEntry> audio;
    render::FrameCache cache;

    explicit State(std::size_t budget) : cache{budget} {}
};

ProjectMediaSource::ProjectMediaSource() = default;
ProjectMediaSource::~ProjectMediaSource() = default;

Result<std::unique_ptr<ProjectMediaSource>> ProjectMediaSource::open(const model::Project& project,
                                                                     std::size_t cacheBudgetBytes) {
    auto source = std::unique_ptr<ProjectMediaSource>(new ProjectMediaSource());
    source->state_ = std::make_unique<State>(cacheBudgetBytes);
    for (const model::MediaRef& ref : project.media()) {
        source->state_->paths[ref.id.value()] = ref.path;
    }
    return source;
}

const render::FrameCache& ProjectMediaSource::cache() const {
    return state_->cache;
}

Result<const render::RgbaImage*> ProjectMediaSource::imageFor(
    model::MediaRefId media, const time::RationalTime& sourceTime) {
    if (const render::RgbaImage* cached = state_->cache.find(media, sourceTime)) {
        return cached;
    }

    const auto path = state_->paths.find(media.value());
    if (path == state_->paths.end()) {
        return Error{ErrorCode::NotFound, "this project has no media with that id"};
    }

    auto& entry = state_->video[media.value()];
    if (!entry.decoder) {
        auto opened = openVideoDecoder(path->second);
        if (!opened) {
            return opened.error();
        }
        entry.decoder = std::move(*opened);
    }

    auto decoded = entry.decoder->frameAtTime(sourceTime);
    if (!decoded) {
        return decoded.error();
    }

    render::RgbaImage image;
    if (Status status = render::toLinear(*decoded, image); !status) {
        return status.error();
    }
    const render::RgbaImage* stored = state_->cache.insert(media, sourceTime, std::move(image));
    if (stored == nullptr) {
        return Error{ErrorCode::Internal,
                     "the frame is larger than the entire cache budget; raise it"};
    }
    return stored;
}

Result<const media::VideoFrame*> ProjectMediaSource::sourceFrameFor(
    model::MediaRefId media, const time::RationalTime& sourceTime) {
    const auto path = state_->paths.find(media.value());
    if (path == state_->paths.end()) {
        return Error{ErrorCode::NotFound, "this project has no media with that id"};
    }

    auto& entry = state_->video[media.value()];
    if (!entry.decoder) {
        auto opened = openVideoDecoder(path->second);
        if (!opened) {
            return opened.error();
        }
        entry.decoder = std::move(*opened);
    }

    auto decoded = entry.decoder->frameAtTime(sourceTime);
    if (!decoded) {
        return decoded.error();
    }
    // No working-space cache here: the GPU converts on upload, so the frame the
    // caller wants is the one the decoder just produced.
    state_->lastSourceFrame = std::move(*decoded);
    return &state_->lastSourceFrame;
}

Status ProjectMediaSource::read(model::MediaRefId media, const time::RationalTime& sourceStart,
                                std::int64_t sampleCount, const time::Rational& sampleRate,
                                media::AudioBuffer& out) {
    const auto path = state_->paths.find(media.value());
    if (path == state_->paths.end()) {
        return Error{ErrorCode::NotFound, "this project has no media with that id"};
    }

    auto& entry = state_->audio[media.value()];
    if (!entry.decoder) {
        auto opened = openAudioDecoder(path->second, {}, sampleRate);
        if (!opened) {
            return opened.error();
        }
        entry.decoder = std::move(*opened);
        entry.position = -1;
    }

    const std::int64_t wantedStart = sourceStart.rescaledTo(sampleRate).frames();

    // Only seek when the request is not where the decoder already sits. A
    // render walks each clip forward, so the common case is a continuation and
    // costs nothing; seeking every block would re-prime the decoder constantly.
    if (entry.position != wantedStart) {
        // Land early and discard: audio seeks are only accurate to a packet, so
        // arriving before the target and walking in is the only way to be
        // sample-exact.
        const time::RationalTime landing{
            std::max<std::int64_t>(0, wantedStart - sampleRate.roundToInt() / 2), sampleRate};
        if (Status status = entry.decoder->seek(landing); !status) {
            return status;
        }
        entry.pending = media::AudioBuffer{};
        entry.pendingOffset = 0;
        entry.position = -1;

        while (true) {
            auto block = entry.decoder->nextBuffer();
            if (!block) {
                return block.error();
            }
            const std::int64_t blockStart = block->pts().rescaledTo(sampleRate).frames();
            const std::int64_t blockEnd = blockStart + block->sampleCount();
            if (blockEnd > wantedStart) {
                entry.pending = std::move(*block);
                entry.pendingOffset = std::max<std::int64_t>(0, wantedStart - blockStart);
                entry.position = wantedStart;
                break;
            }
        }
    }

    const std::int32_t channels = entry.pending.isValid() ? entry.pending.channelCount() : 2;
    out = media::AudioBuffer{channels, sampleCount, sampleRate};

    std::int64_t filled = 0;
    while (filled < sampleCount) {
        if (!entry.pending.isValid() || entry.pendingOffset >= entry.pending.sampleCount()) {
            auto block = entry.decoder->nextBuffer();
            if (!block) {
                // Ran out of media. The remainder stays silent, which is the
                // contract: a clip trimmed to the last sample must not fail the
                // whole mix.
                break;
            }
            entry.pending = std::move(*block);
            entry.pendingOffset = 0;
        }
        const std::int64_t available = entry.pending.sampleCount() - entry.pendingOffset;
        const std::int64_t take = std::min(available, sampleCount - filled);
        for (std::int32_t channel = 0; channel < out.channelCount(); ++channel) {
            const std::int32_t from = std::min(channel, entry.pending.channelCount() - 1);
            const float* in = entry.pending.channel(from) + entry.pendingOffset;
            std::copy_n(in, take, out.channel(channel) + filled);
        }
        entry.pendingOffset += take;
        filled += take;
    }

    entry.position = wantedStart + filled;
    return {};
}

}  // namespace zaro::platform::ffmpeg
