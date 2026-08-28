#include <cstdint>
#include <vector>

#include "zaro/platform/ffmpeg/FFmpegMedia.h"

#include "Ffi.h"

namespace zaro::platform::ffmpeg {
namespace {

class FFmpegAudioDecoder final : public media::AudioDecoder {
public:
    static Result<std::unique_ptr<media::AudioDecoder>> open(
        const std::string& path, const media::DecoderOptions& options,
        std::optional<time::Rational> outputSampleRate);

    [[nodiscard]] const media::AudioStreamInfo& info() const override { return info_; }
    [[nodiscard]] const time::Rational& outputSampleRate() const override { return outputRate_; }

    [[nodiscard]] Result<media::AudioBuffer> nextBuffer() override;
    [[nodiscard]] Status seek(const time::RationalTime& t) override;

private:
    [[nodiscard]] Status pull();
    [[nodiscard]] Result<media::AudioBuffer> convert(const AVFrame& frame);

    FormatContextPtr format_;
    CodecContextPtr codec_;
    SwrContextPtr resampler_;
    FramePtr working_;
    PacketPtr packet_;

    media::AudioStreamInfo info_;
    std::int32_t streamIndex_{-1};
    AVRational timeBase_{};
    time::Rational outputRate_{48000, 1};
    bool needsResample_{false};
};

Result<std::unique_ptr<media::AudioDecoder>> FFmpegAudioDecoder::open(
    const std::string& path, const media::DecoderOptions& options,
    std::optional<time::Rational> outputSampleRate) {
    auto probed = probe(path);
    if (!probed) {
        return probed.error();
    }
    if (probed->audioStreams.empty()) {
        return Error{ErrorCode::NotFound, path + " has no audio stream"};
    }

    auto decoder = std::unique_ptr<FFmpegAudioDecoder>(new FFmpegAudioDecoder());

    const media::AudioStreamInfo* chosen = nullptr;
    for (const auto& candidate : probed->audioStreams) {
        if (options.streamIndex < 0 || candidate.streamIndex == options.streamIndex) {
            chosen = &candidate;
            break;
        }
    }
    if (chosen == nullptr) {
        return Error{ErrorCode::NotFound, "no audio stream matching the requested index"};
    }
    decoder->info_ = *chosen;
    decoder->streamIndex_ = chosen->streamIndex;
    decoder->outputRate_ = outputSampleRate.value_or(chosen->sampleRate);

    AVFormatContext* raw = nullptr;
    if (const int rc = avformat_open_input(&raw, path.c_str(), nullptr, nullptr); rc < 0) {
        return toError(rc, "opening " + path);
    }
    decoder->format_.reset(raw);
    if (const int rc = avformat_find_stream_info(decoder->format_.get(), nullptr); rc < 0) {
        return toError(rc, "reading stream info from " + path);
    }

    AVStream* stream = decoder->format_->streams[decoder->streamIndex_];
    decoder->timeBase_ = stream->time_base;

    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        return Error{ErrorCode::Unsupported, "no decoder for " + decoder->info_.codecName};
    }
    decoder->codec_.reset(avcodec_alloc_context3(codec));
    if (!decoder->codec_) {
        return Error{ErrorCode::Internal, "out of memory allocating a codec context"};
    }
    if (const int rc = avcodec_parameters_to_context(decoder->codec_.get(), stream->codecpar);
        rc < 0) {
        return toError(rc, "configuring the audio decoder");
    }
    decoder->codec_->pkt_timebase = stream->time_base;
    if (const int rc = avcodec_open2(decoder->codec_.get(), codec, nullptr); rc < 0) {
        return toError(rc, "opening the audio decoder");
    }

    decoder->working_ = makeFrame();
    decoder->packet_ = makePacket();
    if (!decoder->working_ || !decoder->packet_) {
        return Error{ErrorCode::Internal, "out of memory allocating decoder scratch"};
    }

    decoder->needsResample_ = decoder->outputRate_ != chosen->sampleRate;
    return std::unique_ptr<media::AudioDecoder>{decoder.release()};
}

Status FFmpegAudioDecoder::pull() {
    while (true) {
        av_frame_unref(working_.get());
        int rc = avcodec_receive_frame(codec_.get(), working_.get());
        if (rc == 0) {
            return {};
        }
        if (rc == AVERROR_EOF) {
            return Error{ErrorCode::EndOfStream, "no more audio"};
        }
        if (rc != AVERROR(EAGAIN)) {
            return toError(rc, "receiving an audio frame");
        }

        bool sent = false;
        while (!sent) {
            av_packet_unref(packet_.get());
            rc = av_read_frame(format_.get(), packet_.get());
            if (rc == AVERROR_EOF) {
                avcodec_send_packet(codec_.get(), nullptr);
                sent = true;
                break;
            }
            if (rc < 0) {
                return toError(rc, "reading an audio packet");
            }
            if (packet_->stream_index != streamIndex_) {
                continue;
            }
            if (const int send = avcodec_send_packet(codec_.get(), packet_.get()); send < 0) {
                return toError(send, "sending an audio packet");
            }
            sent = true;
        }
    }
}

Result<media::AudioBuffer> FFmpegAudioDecoder::convert(const AVFrame& frame) {
    const std::int32_t channels = frame.ch_layout.nb_channels;

    // Everything lands as planar float at the output rate. One layout above this
    // line means the mixer never branches on sample format again.
    if (!resampler_) {
        SwrContext* raw = nullptr;
        AVChannelLayout outLayout{};
        av_channel_layout_copy(&outLayout, &frame.ch_layout);
        const int rc = swr_alloc_set_opts2(
            &raw, &outLayout, AV_SAMPLE_FMT_FLTP, static_cast<int>(outputRate_.roundToInt()),
            &frame.ch_layout, static_cast<AVSampleFormat>(frame.format), frame.sample_rate, 0,
            nullptr);
        av_channel_layout_uninit(&outLayout);
        if (rc < 0) {
            return toError(rc, "configuring the resampler");
        }
        resampler_.reset(raw);
        if (const int init = swr_init(resampler_.get()); init < 0) {
            return toError(init, "initialising the resampler");
        }
    }

    const std::int64_t outSamples = swr_get_out_samples(resampler_.get(), frame.nb_samples);
    media::AudioBuffer buffer{channels, outSamples, outputRate_};

    std::vector<std::uint8_t*> planes(static_cast<std::size_t>(channels));
    for (std::int32_t i = 0; i < channels; ++i) {
        planes[static_cast<std::size_t>(i)] = reinterpret_cast<std::uint8_t*>(buffer.channel(i));
    }

    const int written =
        swr_convert(resampler_.get(), planes.data(), static_cast<int>(outSamples),
                    const_cast<const std::uint8_t**>(frame.extended_data), frame.nb_samples);
    if (written < 0) {
        return toError(written, "resampling audio");
    }
    buffer.resize(written);

    if (frame.pts != kNoPts) {
        const time::Rational seconds = fromAv(timeBase_) * time::Rational::fromInt(frame.pts);
        buffer.setPts(time::RationalTime::fromSeconds(seconds, outputRate_));
    }
    return buffer;
}

Result<media::AudioBuffer> FFmpegAudioDecoder::nextBuffer() {
    if (Status status = pull(); !status) {
        return status.error();
    }
    return convert(*working_);
}

Status FFmpegAudioDecoder::seek(const time::RationalTime& t) {
    const time::Rational inTimeBase = t.toSeconds() / fromAv(timeBase_);
    if (const int rc = av_seek_frame(format_.get(), streamIndex_, inTimeBase.floorToInt(),
                                     AVSEEK_FLAG_BACKWARD);
        rc < 0) {
        return toError(rc, "seeking audio");
    }
    avcodec_flush_buffers(codec_.get());
    return {};
}

}  // namespace

Result<std::unique_ptr<media::AudioDecoder>> openAudioDecoder(
    const std::string& path, const media::DecoderOptions& options,
    std::optional<time::Rational> outputSampleRate) {
    return FFmpegAudioDecoder::open(path, options, std::move(outputSampleRate));
}

}  // namespace zaro::platform::ffmpeg
