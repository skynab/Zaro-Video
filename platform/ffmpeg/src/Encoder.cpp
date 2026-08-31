#include <cstdint>
#include <vector>

#include "zaro/core/render/ColorPipeline.h"
#include "zaro/core/render/ToneMap.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"

#include "Ffi.h"

namespace zaro::platform::ffmpeg {
namespace {

/// The encoder's preferred sample format.
///
/// AVCodec::sample_fmts was deprecated in FFmpeg 7.1 in favour of
/// avcodec_get_supported_config. Both are supported here so the Linux builds,
/// which trail Homebrew by a release or two, still compile.
AVSampleFormat preferredSampleFormat(const AVCodec* codec) {
#if LIBAVCODEC_VERSION_MAJOR > 61 || \
    (LIBAVCODEC_VERSION_MAJOR == 61 && LIBAVCODEC_VERSION_MINOR >= 13)
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configs,
                                     &count) >= 0 &&
        configs != nullptr && count > 0) {
        return static_cast<const AVSampleFormat*>(configs)[0];
    }
#else
    if (codec->sample_fmts != nullptr) {
        return codec->sample_fmts[0];
    }
#endif
    return AV_SAMPLE_FMT_FLTP;
}

bool endsWith(const std::string& text, const char* suffix) {
    const std::string tail{suffix};
    return text.size() >= tail.size() &&
           text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

}  // namespace

struct Encoder::State {
    AVFormatContext* format{nullptr};
    CodecContextPtr videoCodec;
    CodecContextPtr audioCodec;
    AVStream* videoStream{nullptr};
    AVStream* audioStream{nullptr};

    FramePtr videoFrame;
    FramePtr audioFrame;
    PacketPtr packet;
    SwsContextPtr scaler;
    SwrContextPtr resampler;

    std::vector<std::uint8_t> rgb;
    /// Scratch for the rolloff, kept between frames: a tone-mapped copy is
    /// frame-sized and an export allocating one per frame would spend more time
    /// in the allocator than in the encoder.
    render::RgbaImage toneMapped;
    EncodeSettings settings;
    std::int64_t videoPts{0};
    std::int64_t videoPackets{0};
    std::int64_t audioPts{0};
    bool headerWritten{false};
    bool finished{false};

    ~State() {
        if (format != nullptr) {
            if (headerWritten && !finished) {
                av_write_trailer(format);
            }
            if ((format->oformat->flags & AVFMT_NOFILE) == 0 && format->pb != nullptr) {
                avio_closep(&format->pb);
            }
            avformat_free_context(format);
        }
    }
};

Encoder::Encoder() = default;
Encoder::~Encoder() = default;

Result<std::unique_ptr<Encoder>> Encoder::open(const EncodeSettings& settings) {
    if (settings.width <= 0 || settings.height <= 0) {
        return Error{ErrorCode::InvalidData, "the output has no frame size"};
    }
    // Encoders overwhelmingly require even dimensions for subsampled formats,
    // and a confusing failure deep inside libavcodec is worse than saying so.
    if (settings.width % 2 != 0 || settings.height % 2 != 0) {
        return Error{ErrorCode::Unsupported, "output dimensions must be even"};
    }

    auto encoder = std::unique_ptr<Encoder>(new Encoder());
    encoder->state_ = std::make_unique<State>();
    State& state = *encoder->state_;

    if (const int rc =
            avformat_alloc_output_context2(&state.format, nullptr, nullptr, settings.path.c_str());
        rc < 0 || state.format == nullptr) {
        return toError(rc, "choosing a container for " + settings.path);
    }

    const bool isMov = endsWith(settings.path, ".mov");
    const std::string videoName =
        !settings.videoCodec.empty() ? settings.videoCodec : (isMov ? "prores_ks" : "libx264");
    // PCM by default in MOV: it has no encoder delay and no padding, so the
    // sample count out equals the sample count in and drift is measurable
    // rather than merely plausible.
    const std::string audioName =
        !settings.audioCodec.empty() ? settings.audioCodec : (isMov ? "pcm_s16le" : "aac");

    const AVCodec* videoCodec = avcodec_find_encoder_by_name(videoName.c_str());
    if (videoCodec == nullptr) {
        return Error{ErrorCode::Unsupported,
                     "this FFmpeg build has no encoder '" + videoName + "'"};
    }

    state.videoStream = avformat_new_stream(state.format, nullptr);
    state.videoCodec.reset(avcodec_alloc_context3(videoCodec));
    if (state.videoStream == nullptr || !state.videoCodec) {
        return Error{ErrorCode::Internal, "out of memory setting up the video stream"};
    }

    AVCodecContext& video = *state.videoCodec;
    video.width = settings.width;
    video.height = settings.height;
    video.time_base = AVRational{static_cast<int>(settings.frameRate.den()),
                                 static_cast<int>(settings.frameRate.num())};
    video.framerate = toAv(settings.frameRate);
    const bool prores = videoName.find("prores") != std::string::npos;
    if (settings.alpha && !prores) {
        return Error{ErrorCode::Unsupported,
                     "only ProRes 4444 carries an alpha channel here; asking for one from '" +
                         videoName +
                         "' would composite the picture onto black and look like it worked"};
    }
    // 4444 when the coverage is wanted, and its own profile with it: prores_ks
    // picks a profile from the pixel format, and being explicit is the
    // difference between a file that carries alpha and one that quietly does
    // not.
    video.pix_fmt = settings.alpha ? AV_PIX_FMT_YUVA444P10LE
                    : prores       ? AV_PIX_FMT_YUV422P10LE
                                   : AV_PIX_FMT_YUV420P;
    if (settings.alpha) {
        video.profile = 4;  // FF_PROFILE_PRORES_4444
    }
    video.colorspace = AVCOL_SPC_BT709;
    video.color_primaries = AVCOL_PRI_BT709;
    video.color_trc = AVCOL_TRC_BT709;
    video.color_range = AVCOL_RANGE_MPEG;
    if (settings.videoBitRate > 0) {
        video.bit_rate = settings.videoBitRate;
    }
    if ((state.format->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        video.flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (const int rc = avcodec_open2(state.videoCodec.get(), videoCodec, nullptr); rc < 0) {
        return toError(rc, "opening the video encoder");
    }
    if (const int rc =
            avcodec_parameters_from_context(state.videoStream->codecpar, state.videoCodec.get());
        rc < 0) {
        return toError(rc, "describing the video stream");
    }
    state.videoStream->time_base = video.time_base;

    if (settings.includeAudio) {
        const AVCodec* audioCodec = avcodec_find_encoder_by_name(audioName.c_str());
        if (audioCodec == nullptr) {
            return Error{ErrorCode::Unsupported,
                         "this FFmpeg build has no encoder '" + audioName + "'"};
        }
        state.audioStream = avformat_new_stream(state.format, nullptr);
        state.audioCodec.reset(avcodec_alloc_context3(audioCodec));
        if (state.audioStream == nullptr || !state.audioCodec) {
            return Error{ErrorCode::Internal, "out of memory setting up the audio stream"};
        }

        AVCodecContext& audio = *state.audioCodec;
        audio.sample_rate = static_cast<int>(settings.audioSampleRate.roundToInt());
        av_channel_layout_default(&audio.ch_layout, settings.audioChannels);
        audio.sample_fmt = preferredSampleFormat(audioCodec);
        audio.time_base = AVRational{1, audio.sample_rate};
        if ((state.format->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
            audio.flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        if (const int rc = avcodec_open2(state.audioCodec.get(), audioCodec, nullptr); rc < 0) {
            return toError(rc, "opening the audio encoder");
        }
        if (const int rc = avcodec_parameters_from_context(state.audioStream->codecpar,
                                                           state.audioCodec.get());
            rc < 0) {
            return toError(rc, "describing the audio stream");
        }
        state.audioStream->time_base = audio.time_base;
    }

    if ((state.format->oformat->flags & AVFMT_NOFILE) == 0) {
        if (const int rc = avio_open(&state.format->pb, settings.path.c_str(), AVIO_FLAG_WRITE);
            rc < 0) {
            return toError(rc, "opening " + settings.path + " for writing");
        }
    }
    if (const int rc = avformat_write_header(state.format, nullptr); rc < 0) {
        return toError(rc, "writing the container header");
    }
    state.headerWritten = true;

    state.videoFrame = makeFrame();
    state.audioFrame = makeFrame();
    state.packet = makePacket();
    if (!state.videoFrame || !state.audioFrame || !state.packet) {
        return Error{ErrorCode::Internal, "out of memory allocating encoder scratch"};
    }
    state.videoFrame->format = video.pix_fmt;
    state.videoFrame->width = video.width;
    state.videoFrame->height = video.height;
    if (const int rc = av_frame_get_buffer(state.videoFrame.get(), 32); rc < 0) {
        return toError(rc, "allocating an encoder frame");
    }
    state.settings = settings;
    // Four components when the coverage is kept, three otherwise. Sized from
    // the same flag the writer reads, so the buffer cannot be a component
    // narrower than what is about to be written into it.
    state.rgb.resize(static_cast<std::size_t>(settings.width) * (settings.alpha ? 4U : 3U) *
                     static_cast<std::size_t>(settings.height));
    return encoder;
}

namespace {

/// Pull finished packets out of an encoder and hand them to the muxer.
///
/// `defaultDuration` is in the codec's timebase and fills in for encoders that
/// do not set one. It matters more than it looks: the muxer otherwise infers
/// each packet's duration from the gap to the next, and the final packet has no
/// next -- so it lands in the file with a duration of zero, inside the sample
/// index but outside the stream's declared duration. The file then looks
/// complete and decodes one frame short.
Status drain(AVFormatContext* format, AVCodecContext* codec, AVStream* stream, AVPacket* packet,
             std::int64_t defaultDuration, std::int64_t* counter = nullptr) {
    while (true) {
        const int rc = avcodec_receive_packet(codec, packet);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            return {};
        }
        if (rc < 0) {
            return toError(rc, "receiving an encoded packet");
        }
        if (packet->duration == 0) {
            packet->duration = defaultDuration;
        }
        av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
        packet->stream_index = stream->index;
        if (counter != nullptr) {
            ++(*counter);
        }
        const int written = av_interleaved_write_frame(format, packet);
        av_packet_unref(packet);
        if (written < 0) {
            return toError(written, "writing a packet");
        }
    }
}

}  // namespace

Status Encoder::writeVideo(const render::RgbaImage& frame) {
    State& state = *state_;
    if (!frame.isValid()) {
        return Error{ErrorCode::InvalidData, "cannot encode an invalid frame"};
    }
    if (frame.width() != state.videoCodec->width || frame.height() != state.videoCodec->height) {
        return Error{ErrorCode::InvalidData, "frame size does not match the output"};
    }

    // Tone mapped before encoding, not inside it. The encoder's job is to
    // write what it is given and clip what does not fit; making sure nothing
    // needs clipping is a separate decision, and keeping the two apart is what
    // lets a project with nothing above white go through unchanged.
    const render::RgbaImage* toEncode = &frame;
    if (state.settings.highlightKnee < 1.0) {
        state.toneMapped = frame.clone();
        render::toneMap(state.toneMapped, static_cast<float>(state.settings.highlightKnee));
        toEncode = &state.toneMapped;
    }

    const bool keepAlpha = state.settings.alpha;
    const std::int32_t components = keepAlpha ? 4 : 3;
    const std::int32_t stride = frame.width() * components;
    if (Status status = keepAlpha ? render::toDisplayRgba32(*toEncode, state.rgb.data(), stride,
                                                            state.settings.transfer)
                                  : render::toDisplayRgb24(*toEncode, state.rgb.data(), stride,
                                                           state.settings.transfer);
        !status) {
        return status;
    }

    state.scaler.reset(sws_getCachedContext(
        state.scaler.release(), frame.width(), frame.height(),
        keepAlpha ? AV_PIX_FMT_RGBA : AV_PIX_FMT_RGB24, frame.width(), frame.height(),
        state.videoCodec->pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr));
    if (!state.scaler) {
        return Error{ErrorCode::Unsupported, "no conversion to the encoder's pixel format"};
    }
    // RGB in is full range; the encoder is told the output is limited, so the
    // conversion has to be told as well or every export is washed out.
    sws_setColorspaceDetails(state.scaler.get(), sws_getCoefficients(SWS_CS_DEFAULT), 1,
                             sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16, 1 << 16);

    if (const int rc = av_frame_make_writable(state.videoFrame.get()); rc < 0) {
        return toError(rc, "preparing an encoder frame");
    }
    const std::uint8_t* sourcePlanes[1] = {state.rgb.data()};
    const int sourceStrides[1] = {stride};
    if (const int rc = sws_scale(state.scaler.get(), sourcePlanes, sourceStrides, 0, frame.height(),
                                 state.videoFrame->data, state.videoFrame->linesize);
        rc < 0) {
        return toError(rc, "converting for the encoder");
    }

    // The timestamp is the frame counter, not a running sum of durations.
    // Nothing accumulates, so nothing can drift.
    state.videoFrame->pts = state.videoPts++;
    if (const int rc = avcodec_send_frame(state.videoCodec.get(), state.videoFrame.get()); rc < 0) {
        return toError(rc, "encoding a video frame");
    }
    // One frame, in the video codec's timebase.
    return drain(state.format, state.videoCodec.get(), state.videoStream, state.packet.get(), 1,
                 &state.videoPackets);
}

Status Encoder::writeAudio(const media::AudioBuffer& samples) {
    State& state = *state_;
    if (state.audioStream == nullptr) {
        return Error{ErrorCode::InvalidData, "this output has no audio stream"};
    }
    if (samples.sampleCount() == 0) {
        return {};
    }

    AVCodecContext& codec = *state.audioCodec;
    if (!state.resampler) {
        SwrContext* raw = nullptr;
        AVChannelLayout inLayout{};
        av_channel_layout_default(&inLayout, samples.channelCount());
        const int rc = swr_alloc_set_opts2(
            &raw, &codec.ch_layout, codec.sample_fmt, codec.sample_rate, &inLayout,
            AV_SAMPLE_FMT_FLTP, static_cast<int>(samples.sampleRate().roundToInt()), 0, nullptr);
        av_channel_layout_uninit(&inLayout);
        if (rc < 0) {
            return toError(rc, "configuring the audio output converter");
        }
        state.resampler.reset(raw);
        if (const int init = swr_init(state.resampler.get()); init < 0) {
            return toError(init, "initialising the audio output converter");
        }
    }

    // Some encoders demand a fixed block size; PCM and the like accept any.
    const int blockSize =
        codec.frame_size > 0 ? codec.frame_size : static_cast<int>(samples.sampleCount());

    std::vector<const std::uint8_t*> inputPlanes(static_cast<std::size_t>(samples.channelCount()));
    for (std::int32_t channel = 0; channel < samples.channelCount(); ++channel) {
        inputPlanes[static_cast<std::size_t>(channel)] =
            reinterpret_cast<const std::uint8_t*>(samples.channel(channel));
    }

    // Feed the converter, then pull fixed-size blocks out of it. swr buffers the
    // remainder internally, so a block boundary never falls between two calls.
    if (const int rc = swr_convert(state.resampler.get(), nullptr, 0, inputPlanes.data(),
                                   static_cast<int>(samples.sampleCount()));
        rc < 0) {
        return toError(rc, "buffering audio for the encoder");
    }

    while (swr_get_out_samples(state.resampler.get(), 0) >= blockSize) {
        av_frame_unref(state.audioFrame.get());
        state.audioFrame->format = codec.sample_fmt;
        state.audioFrame->nb_samples = blockSize;
        if (const int rc = av_channel_layout_copy(&state.audioFrame->ch_layout, &codec.ch_layout);
            rc < 0) {
            return toError(rc, "setting the audio channel layout");
        }
        if (const int rc = av_frame_get_buffer(state.audioFrame.get(), 0); rc < 0) {
            return toError(rc, "allocating an audio frame");
        }
        const int produced =
            swr_convert(state.resampler.get(), state.audioFrame->data, blockSize, nullptr, 0);
        if (produced < 0) {
            return toError(produced, "converting audio for the encoder");
        }
        if (produced == 0) {
            break;
        }
        state.audioFrame->nb_samples = produced;
        state.audioFrame->pts = state.audioPts;
        state.audioPts += produced;

        if (const int rc = avcodec_send_frame(state.audioCodec.get(), state.audioFrame.get());
            rc < 0) {
            return toError(rc, "encoding audio");
        }
        if (Status status = drain(state.format, state.audioCodec.get(), state.audioStream,
                                  state.packet.get(), produced);
            !status) {
            return status;
        }
    }
    return {};
}

Status Encoder::finish() {
    State& state = *state_;
    if (state.finished) {
        return {};
    }

    if (state.resampler) {
        // Whatever the converter is still holding, padded out to one last block.
        while (swr_get_out_samples(state.resampler.get(), 0) > 0) {
            const int remaining = swr_get_out_samples(state.resampler.get(), 0);
            av_frame_unref(state.audioFrame.get());
            state.audioFrame->format = state.audioCodec->sample_fmt;
            state.audioFrame->nb_samples = remaining;
            if (av_channel_layout_copy(&state.audioFrame->ch_layout, &state.audioCodec->ch_layout) <
                0) {
                break;
            }
            if (av_frame_get_buffer(state.audioFrame.get(), 0) < 0) {
                break;
            }
            const int produced =
                swr_convert(state.resampler.get(), state.audioFrame->data, remaining, nullptr, 0);
            if (produced <= 0) {
                break;
            }
            state.audioFrame->nb_samples = produced;
            state.audioFrame->pts = state.audioPts;
            state.audioPts += produced;
            if (avcodec_send_frame(state.audioCodec.get(), state.audioFrame.get()) < 0) {
                break;
            }
            if (Status status = drain(state.format, state.audioCodec.get(), state.audioStream,
                                      state.packet.get(), produced);
                !status) {
                return status;
            }
        }
    }

    if (state.videoCodec) {
        if (const int rc = avcodec_send_frame(state.videoCodec.get(), nullptr); rc < 0) {
            return toError(rc, "flushing the video encoder");
        }
        if (Status status = drain(state.format, state.videoCodec.get(), state.videoStream,
                                  state.packet.get(), 1, &state.videoPackets);
            !status) {
            return status;
        }
    }
    if (state.audioCodec) {
        avcodec_send_frame(state.audioCodec.get(), nullptr);
        if (Status status = drain(state.format, state.audioCodec.get(), state.audioStream,
                                  state.packet.get(), 0);
            !status) {
            return status;
        }
    }

    if (const int rc = av_write_trailer(state.format); rc < 0) {
        return toError(rc, "finalising the file");
    }
    state.finished = true;
    return {};
}

std::int64_t Encoder::framesWritten() const {
    return state_->videoPts;
}
std::int64_t Encoder::videoPacketsWritten() const {
    return state_->videoPackets;
}
std::int64_t Encoder::samplesWritten() const {
    return state_->audioPts;
}

}  // namespace zaro::platform::ffmpeg
