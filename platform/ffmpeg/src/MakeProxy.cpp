#include <algorithm>
#include <cstdint>
#include <filesystem>

#include "zaro/core/render/Resample.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"

namespace zaro::platform::ffmpeg {
namespace {

/// Round down to an even number, and never below two.
///
/// Every codec worth making a proxy in subsamples chroma, so an odd dimension
/// is not something they can represent -- and the failure is an encoder error
/// halfway through a long job rather than at the start.
[[nodiscard]] std::int32_t even(std::int32_t value) {
    return std::max(2, value - (value % 2));
}

}  // namespace

Result<ProxySummary> makeProxy(const ProxySettings& settings,
                               const std::function<void(double)>& onProgress,
                               const std::function<bool()>& keepGoing) {
    auto probed = probe(settings.source);
    if (!probed) {
        return probed.error();
    }
    const media::VideoStreamInfo* video = probed->primaryVideo();
    if (video == nullptr || video->width <= 0 || video->height <= 0) {
        return Error{ErrorCode::InvalidData, "there is no picture in " + settings.source};
    }
    if (settings.destination.empty()) {
        return Error{ErrorCode::InvalidData, "a proxy needs somewhere to go"};
    }

    const time::Rational rate = video->frameRate;
    const std::int64_t frameCount = video->durationInFrames().frames();
    if (frameCount <= 0) {
        return Error{ErrorCode::InvalidData, "that file has no frames to copy"};
    }

    // A project of one file, so the proxy is decoded through exactly the path
    // a render uses. No transfer override is set on it: the decode then reads
    // the file's own tag and the encode writes the same one back, which makes
    // the round trip carry the original's code values rather than reinterpret
    // them. Whatever the person editing says the footage really is then applies
    // to the proxy the same way it applies to the original.
    model::Project holder;
    model::MediaRef ref;
    ref.id = holder.ids().next<model::MediaRefTag>();
    ref.path = settings.source;
    ref.name = std::filesystem::path{settings.source}.filename().string();
    ref.info = *probed;
    const model::MediaRefId mediaId = holder.addMedia(ref);

    auto sourceOpened = ProjectMediaSource::open(holder);
    if (!sourceOpened) {
        return sourceOpened.error();
    }
    ProjectMediaSource& source = **sourceOpened;

    ProxySummary summary;
    summary.path = settings.destination;
    // Zero means the source's own size: an ingest transcode changes the codec
    // and nothing else.
    summary.width =
        even(settings.width > 0 ? std::min(settings.width, video->width) : video->width);
    summary.height = even(static_cast<std::int32_t>(
        std::llround(static_cast<double>(summary.width) * static_cast<double>(video->height) /
                     static_cast<double>(video->width))));

    const media::AudioStreamInfo* audio = probed->primaryAudio();
    EncodeSettings encode;
    encode.path = settings.destination;
    encode.width = summary.width;
    encode.height = summary.height;
    encode.frameRate = rate;
    // H.264 and AAC unless somebody says otherwise, rather than the
    // container's defaults. A .mov defaults to ProRes and PCM, which is right
    // for a deliverable and absurd for a proxy: the first version of this made
    // proxies eight times the size of the H.264 originals they stood in for,
    // which is the opposite of the point.
    encode.videoCodec = settings.videoCodec.empty() ? "libx264" : settings.videoCodec;
    encode.audioCodec = "aac";
    encode.videoBitRate = settings.videoBitRate;
    encode.includeAudio = audio != nullptr && audio->channelCount > 0;
    if (encode.includeAudio) {
        encode.audioSampleRate = audio->sampleRate;
        encode.audioChannels = audio->channelCount;
    }
    encode.transfer = video->color.transfer == media::TransferFunction::Unknown
                          ? media::TransferFunction::BT709
                          : video->color.transfer;

    auto encoderOpened = Encoder::open(encode);
    if (!encoderOpened) {
        return encoderOpened.error();
    }
    Encoder& encoder = **encoderOpened;

    // The same exact-rational relationship the renderer uses, for the same
    // reason: accumulating a per-frame duration drifts by a sample every few
    // frames at 29.97.
    const time::Rational samplesPerFrame = encode.audioSampleRate / rate;
    const auto sampleAtFrame = [&](std::int64_t frame) {
        return (time::Rational::fromInt(frame) * samplesPerFrame).floorToInt();
    };

    render::RgbaImage scaled{summary.width, summary.height};
    media::AudioBuffer block;
    for (std::int64_t index = 0; index < frameCount; ++index) {
        if (keepGoing && !keepGoing()) {
            std::error_code code;
            std::filesystem::remove(settings.destination, code);
            return Error{ErrorCode::Cancelled, "proxy abandoned"};
        }
        auto frame = source.imageFor(mediaId, time::RationalTime{index, rate});
        if (!frame) {
            // A frame that cannot be read is written as the last one that
            // could, rather than abandoning: the proxy has to have exactly as
            // many frames as the original or it retimes every cut made on it.
            // A repeated frame is visible; a shorter file is not.
            if (index == 0) {
                return frame.error();
            }
        } else if ((*frame)->width() == scaled.width() && (*frame)->height() == scaled.height()) {
            // Same size: resampling would be an expensive identity, and a
            // box average at 1:1 is still a box average.
            scaled = (*frame)->clone();
        } else {
            render::resizeInto(**frame, scaled);
        }
        if (Status status = encoder.writeVideo(scaled); !status) {
            return status.error();
        }

        if (encode.includeAudio) {
            const std::int64_t from = sampleAtFrame(index);
            const std::int64_t to = sampleAtFrame(index + 1);
            if (Status status =
                    source.read(mediaId, time::RationalTime{from, encode.audioSampleRate},
                                to - from, encode.audioSampleRate, block);
                !status) {
                return status.error();
            }
            if (Status status = encoder.writeAudio(block); !status) {
                return status.error();
            }
        }

        if (onProgress) {
            onProgress(static_cast<double>(index + 1) / static_cast<double>(frameCount));
        }
    }

    if (Status status = encoder.finish(); !status) {
        return status.error();
    }

    summary.frames = encoder.framesWritten();
    std::error_code code;
    summary.sourceBytes =
        static_cast<std::uint64_t>(std::filesystem::file_size(settings.source, code));
    summary.proxyBytes =
        static_cast<std::uint64_t>(std::filesystem::file_size(settings.destination, code));
    return summary;
}

}  // namespace zaro::platform::ffmpeg
