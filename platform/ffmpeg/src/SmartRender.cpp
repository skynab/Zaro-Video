#include "zaro/core/render/SmartRender.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

#include "zaro/platform/ffmpeg/FFmpegRender.h"

#include "Ffi.h"

namespace zaro::platform::ffmpeg {
namespace {

struct OutputDeleter {
    void operator()(AVFormatContext* p) const noexcept {
        if (p != nullptr) {
            if ((p->oformat->flags & AVFMT_NOFILE) == 0 && p->pb != nullptr) {
                avio_closep(&p->pb);
            }
            avformat_free_context(p);
        }
    }
};

}  // namespace

Result<CopySummary> copyRange(const std::string& sourcePath, const std::string& outputPath,
                              const time::RationalTime& sourceStart, std::int64_t frames,
                              bool includeAudio, const std::function<void(std::int64_t)>& onFrames,
                              const std::function<bool()>& keepGoing) {
    std::unique_ptr<AVFormatContext, FormatContextDeleter> input;
    {
        AVFormatContext* opened = nullptr;
        const int status = avformat_open_input(&opened, sourcePath.c_str(), nullptr, nullptr);
        if (status < 0) {
            return toError(status, "opening " + sourcePath);
        }
        input.reset(opened);
    }
    if (const int status = avformat_find_stream_info(input.get(), nullptr); status < 0) {
        return toError(status, "reading the streams of " + sourcePath);
    }

    const int videoIndex = av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIndex < 0) {
        return toError(videoIndex, "finding a picture in " + sourcePath);
    }
    const int audioIndex =
        includeAudio ? av_find_best_stream(input.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0)
                     : -1;

    std::unique_ptr<AVFormatContext, OutputDeleter> output;
    {
        AVFormatContext* created = nullptr;
        const int status =
            avformat_alloc_output_context2(&created, nullptr, nullptr, outputPath.c_str());
        if (status < 0 || created == nullptr) {
            return toError(status, "working out what kind of file " + outputPath + " is");
        }
        output.reset(created);
    }

    // One output stream per input stream being copied, with the codec
    // parameters taken across unchanged: that is what makes this a copy.
    std::map<int, int> streamMap;
    for (const int index : {videoIndex, audioIndex}) {
        if (index < 0) {
            continue;
        }
        AVStream* from = input->streams[index];
        AVStream* to = avformat_new_stream(output.get(), nullptr);
        if (to == nullptr) {
            return Error{ErrorCode::Io, "cannot add a stream to " + outputPath};
        }
        if (const int status = avcodec_parameters_copy(to->codecpar, from->codecpar); status < 0) {
            return toError(status, "copying the codec parameters");
        }
        to->codecpar->codec_tag = 0;  // the muxer picks a tag its container allows
        to->time_base = from->time_base;
        streamMap[index] = to->index;
    }

    if ((output->oformat->flags & AVFMT_NOFILE) == 0) {
        if (const int status = avio_open(&output->pb, outputPath.c_str(), AVIO_FLAG_WRITE);
            status < 0) {
            return toError(status, "opening " + outputPath + " for writing");
        }
    }
    if (const int status = avformat_write_header(output.get(), nullptr); status < 0) {
        return toError(status, "writing the header of " + outputPath);
    }

    AVStream* videoStream = input->streams[videoIndex];
    const AVRational videoTime = videoStream->time_base;
    const time::Rational rate = sourceStart.rate();
    const auto toStreamTime = [&](const time::RationalTime& when) {
        const double seconds = when.toSecondsDouble();
        return static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(videoTime.den) /
                                                      static_cast<double>(videoTime.num)));
    };
    const std::int64_t startPts = toStreamTime(sourceStart);
    const std::int64_t endPts = toStreamTime(sourceStart + time::RationalTime{frames, rate});

    // Seek to the keyframe at or before the start. A copy has to begin on one:
    // starting anywhere else would hand the decoder frames that refer back to
    // pictures the file no longer contains.
    if (const int status = av_seek_frame(input.get(), videoIndex, startPts, AVSEEK_FLAG_BACKWARD);
        status < 0) {
        return toError(status, "seeking " + sourcePath);
    }

    CopySummary summary;
    std::unique_ptr<AVPacket, PacketDeleter> packet{av_packet_alloc()};
    bool sawFirstVideo = false;
    while (av_read_frame(input.get(), packet.get()) >= 0) {
        if (keepGoing && !keepGoing()) {
            av_packet_unref(packet.get());
            return Error{ErrorCode::Cancelled, "copy abandoned"};
        }
        const int index = packet->stream_index;
        const auto mapped = streamMap.find(index);
        if (mapped == streamMap.end()) {
            av_packet_unref(packet.get());
            continue;
        }
        AVStream* from = input->streams[index];
        const std::int64_t inVideoTime = av_rescale_q(
            packet->pts == kNoPts ? packet->dts : packet->pts, from->time_base, videoTime);

        if (index == videoIndex && !sawFirstVideo) {
            if (inVideoTime > startPts) {
                // The seek landed after the start, which means the frame asked
                // for is not a keyframe and the ones before it are gone.
                av_packet_unref(packet.get());
                return Error{ErrorCode::InvalidData,
                             "that cut is not on a keyframe, so it cannot be copied"};
            }
            if (inVideoTime < startPts) {
                // Frames before the range: the file's own keyframe interval put
                // them here, and copying them would make the export longer than
                // it was asked to be.
                av_packet_unref(packet.get());
                continue;
            }
            sawFirstVideo = true;
        }
        if (inVideoTime >= endPts) {
            av_packet_unref(packet.get());
            if (index == videoIndex) {
                break;
            }
            continue;
        }
        if (inVideoTime < startPts) {
            av_packet_unref(packet.get());
            continue;
        }

        // Rebased so the export starts at zero, and rescaled into whatever
        // timebase the muxer chose for the stream.
        AVStream* to = output->streams[mapped->second];
        const std::int64_t offset = av_rescale_q(startPts, videoTime, from->time_base);
        if (packet->pts != kNoPts) {
            packet->pts = av_rescale_q(packet->pts - offset, from->time_base, to->time_base);
        }
        if (packet->dts != kNoPts) {
            packet->dts = av_rescale_q(packet->dts - offset, from->time_base, to->time_base);
        }
        packet->duration = av_rescale_q(packet->duration, from->time_base, to->time_base);
        packet->pos = -1;
        packet->stream_index = to->index;

        if (index == videoIndex) {
            ++summary.videoPackets;
            if (onFrames) {
                onFrames(summary.videoPackets);
            }
        } else {
            ++summary.audioPackets;
        }
        if (const int status = av_interleaved_write_frame(output.get(), packet.get()); status < 0) {
            return toError(status, "writing a packet to " + outputPath);
        }
        av_packet_unref(packet.get());
    }

    if (const int status = av_write_trailer(output.get()); status < 0) {
        return toError(status, "finishing " + outputPath);
    }
    if (summary.videoPackets == 0) {
        return Error{ErrorCode::InvalidData, "nothing in that range could be copied"};
    }
    return summary;
}

}  // namespace zaro::platform::ffmpeg
