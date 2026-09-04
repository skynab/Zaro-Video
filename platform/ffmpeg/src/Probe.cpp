#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "zaro/platform/ffmpeg/FFmpegMedia.h"

#include "Ffi.h"

namespace zaro::platform::ffmpeg {
namespace {

std::int32_t rotationFrom(const AVStream& stream) {
    // The display matrix moved from the stream onto AVCodecParameters in FFmpeg
    // 6.1, which deprecated the old accessor in the same release that added the
    // replacement -- so there is no version where either one alone is right.
    // Support both, so the Linux builds, which trail a release or two behind
    // Homebrew, still work.
    //
    // The test is on libavcodec and not libavformat because that is where both
    // halves of the new API live: av_packet_side_data_get() arrived in lavc
    // 60.29.100 and AVCodecParameters::coded_side_data in 60.30.100, so the
    // later of the two is the boundary. Keying this on a libavformat major
    // instead put the boundary at 7.0 and left every 6.1 build -- Ubuntu 24.04
    // among them -- compiling the deprecated call under -Werror.
    const std::uint8_t* data = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 30, 100)
    const AVCodecParameters& par = *stream.codecpar;
    const AVPacketSideData* side = av_packet_side_data_get(
        par.coded_side_data, par.nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (side != nullptr && side->size >= sizeof(std::int32_t) * 9) {
        data = side->data;
    }
#else
    std::size_t size = 0;
    data = av_stream_get_side_data(&stream, AV_PKT_DATA_DISPLAYMATRIX, &size);
    if (size < sizeof(std::int32_t) * 9) {
        data = nullptr;
    }
#endif
    if (data == nullptr) {
        return 0;
    }
    // av_display_rotation_get returns anticlockwise degrees; the rest of the
    // world, and every UI that will show this, thinks clockwise.
    const double degrees = -av_display_rotation_get(reinterpret_cast<const std::int32_t*>(data));
    if (std::isnan(degrees)) {
        return 0;
    }
    auto normalised = static_cast<std::int32_t>(std::lround(degrees)) % 360;
    if (normalised < 0) {
        normalised += 360;
    }
    return normalised;
}

std::optional<time::Timecode> timecodeFrom(const AVStream& stream, const AVFormatContext& format) {
    const AVDictionaryEntry* entry = av_dict_get(stream.metadata, "timecode", nullptr, 0);
    if (entry == nullptr) {
        entry = av_dict_get(format.metadata, "timecode", nullptr, 0);
    }
    if (entry == nullptr) {
        return std::nullopt;
    }
    return time::parseTimecode(entry->value);
}

/// Whether this container is one of FFmpeg's single-picture demuxers.
///
/// FFmpeg presents a still as a video stream, and the only reliable sign that
/// it is a picture rather than a film is which demuxer opened it: `png_pipe`,
/// `jpeg_pipe`, `image2` and their siblings all exist to hand over exactly one
/// frame. The stream's own numbers cannot be trusted for this -- the rate is
/// invented (25fps, always), the duration is either absent or one frame, and
/// `nb_frames` is usually zero -- so a test built on those would call a
/// one-frame video a still and a still with a duration a video.
[[nodiscard]] bool looksLikeStillContainer(const char* formatName) {
    if (formatName == nullptr) {
        return false;
    }
    const std::string name{formatName};
    // The _pipe demuxers are one per codec -- png_pipe, jpeg_pipe, webp_pipe,
    // bmp_pipe, tiff_pipe and a dozen more -- so they are matched by their
    // shared suffix rather than listed. image2 is the odd one out, and is what
    // a .jpg most often arrives as.
    if (name == "image2" || name == "image2pipe") {
        return true;
    }
    static constexpr std::string_view kPipe{"_pipe"};
    return name.size() > kPipe.size() &&
           name.compare(name.size() - kPipe.size(), kPipe.size(), kPipe) == 0;
}

/// Container-declared rate versus observed average rate. A real divergence is
/// the cheapest available signal that timestamps are not evenly spaced, though
/// it is only a hint -- conform() is what actually settles the question.
bool looksVariableRate(const time::Rational& nominal, const time::Rational& average) {
    if (!nominal.isPositive() || !average.isPositive()) {
        return false;
    }
    const time::Rational difference = (nominal - average).abs();
    return difference * time::Rational::fromInt(1000) > average;
}

}  // namespace

void installLogHandler(bool verbose) {
    av_log_set_level(verbose ? AV_LOG_INFO : AV_LOG_ERROR);
}

Result<media::MediaInfo> probe(const std::string& path) {
    AVFormatContext* raw = nullptr;
    if (const int rc = avformat_open_input(&raw, path.c_str(), nullptr, nullptr); rc < 0) {
        return toError(rc, "opening " + path);
    }
    FormatContextPtr format{raw};

    if (const int rc = avformat_find_stream_info(format.get(), nullptr); rc < 0) {
        return toError(rc, "reading stream info from " + path);
    }

    media::MediaInfo info;
    info.path = path;
    info.formatName = format->iformat != nullptr ? format->iformat->name : "";
    const bool still =
        looksLikeStillContainer(format->iformat != nullptr ? format->iformat->name : nullptr);
    info.bitRate = format->bit_rate;
    if (format->duration != kNoPts) {
        info.duration = time::Rational{format->duration, AV_TIME_BASE};
    }

    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVStream& stream = *format->streams[i];
        const AVCodecParameters& par = *stream.codecpar;
        const AVCodec* codec = avcodec_find_decoder(par.codec_id);
        const std::string codecName =
            codec != nullptr ? codec->name : avcodec_get_name(par.codec_id);

        if (par.codec_type == AVMEDIA_TYPE_VIDEO) {
            // Attached cover art is a single still masquerading as a video
            // stream. Reporting it as footage confuses everything downstream.
            if ((stream.disposition & AV_DISPOSITION_ATTACHED_PIC) != 0) {
                continue;
            }
            media::VideoStreamInfo video;
            video.streamIndex = static_cast<std::int32_t>(i);
            video.codecName = codecName;
            video.width = par.width;
            video.height = par.height;
            video.pixelFormat = fromAv(static_cast<AVPixelFormat>(par.format));

            const media::ColorInfo tagged = colorInfoFrom(par);
            video.colorWasGuessed = !tagged.isFullyTagged();
            video.color = tagged.resolved(par.width, par.height);

            video.frameRate = fromAv(stream.r_frame_rate);
            video.averageFrameRate = fromAv(stream.avg_frame_rate);
            if (!video.frameRate.isPositive()) {
                video.frameRate = video.averageFrameRate;
            }
            if (!video.averageFrameRate.isPositive()) {
                video.averageFrameRate = video.frameRate;
            }
            video.isVariableFrameRate = looksVariableRate(video.frameRate, video.averageFrameRate);

            video.pixelAspect = par.sample_aspect_ratio.num > 0 ? fromAv(par.sample_aspect_ratio)
                                                                : time::Rational{1, 1};
            video.rotationDegrees = rotationFrom(stream);
            video.frameCountHint = stream.nb_frames;
            video.startTimecode = timecodeFrom(stream, *format);

            if (stream.duration != kNoPts) {
                video.duration =
                    fromAv(stream.time_base) * time::Rational::fromInt(stream.duration);
            } else {
                video.duration = info.duration;
            }

            // A still is one frame however the container spells its duration,
            // and its duration on a timeline is whatever somebody stretches it
            // to. Saying so here keeps every caller from having to recognise a
            // picture for itself.
            video.isStill = still;
            if (still) {
                video.frameCountHint = 1;
                video.isVariableFrameRate = false;
            }
            info.videoStreams.push_back(std::move(video));

        } else if (par.codec_type == AVMEDIA_TYPE_AUDIO) {
            media::AudioStreamInfo audio;
            audio.streamIndex = static_cast<std::int32_t>(i);
            audio.codecName = codecName;
            audio.sampleRate = time::Rational::fromInt(par.sample_rate);
            audio.channelCount = par.ch_layout.nb_channels;

            std::array<char, 64> layout{};
            if (av_channel_layout_describe(&par.ch_layout, layout.data(), layout.size()) > 0) {
                audio.channelLayout = layout.data();
            }
            if (stream.duration != kNoPts) {
                audio.duration =
                    fromAv(stream.time_base) * time::Rational::fromInt(stream.duration);
            } else {
                audio.duration = info.duration;
            }
            audio.sampleCountHint =
                audio.duration.isPositive() ? (audio.duration * audio.sampleRate).roundToInt() : 0;
            info.audioStreams.push_back(std::move(audio));
        }
    }

    if (info.videoStreams.empty() && info.audioStreams.empty()) {
        return Error{ErrorCode::Unsupported, path + " has no video or audio streams"};
    }
    return info;
}

}  // namespace zaro::platform::ffmpeg
