#include <algorithm>
#include <cstdint>
#include <vector>

#include "zaro/platform/ffmpeg/FFmpegMedia.h"

#include "Ffi.h"

namespace zaro::platform::ffmpeg {
namespace {

/// How far ahead of the current position a target has to be before seeking
/// beats decoding forward. Seeking costs a keyframe re-entry and a flush;
/// below roughly a second of material, walking forward is cheaper and avoids
/// re-decoding a long GOP from its head.
constexpr std::int64_t kSeekThresholdMicros = 1'000'000;

/// How many times to widen the seek before giving up. Each attempt doubles the
/// distance, so eight covers over two minutes of backing off.
constexpr int kMaxSeekAttempts = 8;

class FFmpegVideoDecoder final : public media::VideoDecoder {
public:
    static Result<std::unique_ptr<media::VideoDecoder>> open(const std::string& path,
                                                             const media::DecoderOptions& options);

    [[nodiscard]] const media::VideoStreamInfo& info() const override { return info_; }
    [[nodiscard]] bool usingHardware() const override { return hardwareActive_; }

    [[nodiscard]] Result<media::VideoFrame> nextFrame() override;
    [[nodiscard]] Result<media::VideoFrame> frameAtTime(const time::RationalTime& t) override;
    [[nodiscard]] Result<media::VideoFrame> frameAtIndex(std::int64_t index) override;
    [[nodiscard]] Status conform() override;
    [[nodiscard]] bool isConformed() const override { return conformed_; }
    [[nodiscard]] Result<std::int64_t> frameCount() override;

private:
    /// Decode the next frame into `working_`, transferring off the GPU if it
    /// came back as a hardware surface. Returns EndOfStream when exhausted.
    [[nodiscard]] Status pull();

    /// The last frame whose timestamp is at or before `target`, in stream
    /// timebase units. If `target` precedes the first frame, the first frame.
    [[nodiscard]] Result<media::VideoFrame> frameAtStreamPts(std::int64_t target);

    /// The one picture in a still, decoded once and kept.
    ///
    /// A still gets a path of its own rather than going through the seek
    /// machinery above, because none of what that machinery is for applies: it
    /// exists to land accurately in a long stream of timestamped frames, and a
    /// .png is one frame with, very often, no timestamp at all. The pipe
    /// demuxers also seek poorly or not at all. So the frame is pulled once
    /// from the top of the file and every later request is answered from it --
    /// which is also the cheapest possible answer, and a still is asked for
    /// once per rendered frame for as long as it is on screen.
    [[nodiscard]] Result<media::VideoFrame> stillPicture();

    [[nodiscard]] Status seekTo(std::int64_t streamPts);

    [[nodiscard]] static std::int64_t timestampOf(const AVFrame& frame) {
        return frame.best_effort_timestamp != kNoPts ? frame.best_effort_timestamp : frame.pts;
    }

    static AVPixelFormat chooseFormat(AVCodecContext* ctx, const AVPixelFormat* formats);

    FormatContextPtr format_;
    CodecContextPtr codec_;
    BufferRefPtr hardwareDevice_;
    SwsContextPtr scaler_;

    /// The decoded still, once something has asked for it.
    media::VideoFrame still_;
    bool haveStill_{false};

    FramePtr working_;   ///< Frame currently being decoded into.
    FramePtr software_;  ///< Landing pad for hardware surface downloads.
    FramePtr pending_;   ///< Read ahead of the target; handed out next.
    PacketPtr packet_;

    media::VideoStreamInfo info_;
    std::int32_t streamIndex_{-1};
    AVRational timeBase_{};

    AVPixelFormat hardwarePixelFormat_{AV_PIX_FMT_NONE};
    bool hardwareActive_{false};
    bool hardwareRequested_{false};

    std::vector<std::int64_t> conformIndex_;
    bool conformed_{false};

    std::int64_t positionPts_{kNoPts};
    std::int64_t firstPts_{0};
    bool havePending_{false};
    bool endOfStream_{false};
};

AVPixelFormat FFmpegVideoDecoder::chooseFormat(AVCodecContext* ctx, const AVPixelFormat* formats) {
    auto* self = static_cast<FFmpegVideoDecoder*>(ctx->opaque);
    for (const AVPixelFormat* candidate = formats; *candidate != AV_PIX_FMT_NONE; ++candidate) {
        if (*candidate == self->hardwarePixelFormat_) {
            return *candidate;
        }
    }
    // The decoder declined hardware for this stream -- an unsupported profile,
    // usually. Record it so usingHardware() reports what actually happened
    // rather than what was asked for.
    self->hardwareActive_ = false;
    return formats[0];
}

Result<std::unique_ptr<media::VideoDecoder>> FFmpegVideoDecoder::open(
    const std::string& path, const media::DecoderOptions& options) {
    auto probed = probe(path);
    if (!probed) {
        return probed.error();
    }
    if (probed->videoStreams.empty()) {
        return Error{ErrorCode::NotFound, path + " has no video stream"};
    }

    auto decoder = std::unique_ptr<FFmpegVideoDecoder>(new FFmpegVideoDecoder());

    const media::VideoStreamInfo* chosen = nullptr;
    for (const auto& candidate : probed->videoStreams) {
        if (options.streamIndex < 0 || candidate.streamIndex == options.streamIndex) {
            chosen = &candidate;
            break;
        }
    }
    if (chosen == nullptr) {
        return Error{ErrorCode::NotFound, "no video stream matching the requested index"};
    }
    decoder->info_ = *chosen;
    decoder->streamIndex_ = chosen->streamIndex;

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
    decoder->firstPts_ = stream->start_time != kNoPts ? stream->start_time : 0;

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
        return toError(rc, "configuring the decoder");
    }
    decoder->codec_->pkt_timebase = stream->time_base;
    decoder->codec_->thread_count = options.threadCount;

    // Auto deliberately resolves to software while the only consumer of a frame
    // is CPU-side; see the comment on DecodeMode::Auto for the measurements.
    // Hardware is still fully wired up and reachable through ForceHardware, so
    // that Phase 3 inherits a working path rather than an untested one.
    if (options.mode == media::DecodeMode::ForceHardware) {
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
            if (config == nullptr) {
                break;
            }
            if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 &&
                config->device_type == AV_HWDEVICE_TYPE_VIDEOTOOLBOX) {
                decoder->hardwarePixelFormat_ = config->pix_fmt;
                break;
            }
        }

        if (decoder->hardwarePixelFormat_ != AV_PIX_FMT_NONE) {
            AVBufferRef* device = nullptr;
            if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr,
                                       0) >= 0) {
                decoder->hardwareDevice_.reset(device);
                decoder->codec_->hw_device_ctx = av_buffer_ref(device);
                decoder->codec_->opaque = decoder.get();
                decoder->codec_->get_format = &FFmpegVideoDecoder::chooseFormat;
                decoder->hardwareActive_ = true;
                decoder->hardwareRequested_ = true;
            }
        }

        if (options.mode == media::DecodeMode::ForceHardware && !decoder->hardwareRequested_) {
            return Error{ErrorCode::Unsupported,
                         "hardware decoding was required but is unavailable for " +
                             decoder->info_.codecName};
        }
    }

    if (const int rc = avcodec_open2(decoder->codec_.get(), codec, nullptr); rc < 0) {
        return toError(rc, "opening the decoder");
    }

    decoder->working_ = makeFrame();
    decoder->software_ = makeFrame();
    decoder->pending_ = makeFrame();
    decoder->packet_ = makePacket();
    if (!decoder->working_ || !decoder->software_ || !decoder->pending_ || !decoder->packet_) {
        return Error{ErrorCode::Internal, "out of memory allocating decoder scratch"};
    }

    return std::unique_ptr<media::VideoDecoder>{decoder.release()};
}

Status FFmpegVideoDecoder::pull() {
    if (havePending_) {
        av_frame_unref(working_.get());
        av_frame_move_ref(working_.get(), pending_.get());
        havePending_ = false;
        positionPts_ = timestampOf(*working_);
        return {};
    }

    while (true) {
        av_frame_unref(working_.get());
        int rc = avcodec_receive_frame(codec_.get(), working_.get());

        if (rc == 0) {
            if (working_->format == hardwarePixelFormat_ &&
                hardwarePixelFormat_ != AV_PIX_FMT_NONE) {
                av_frame_unref(software_.get());
                if (const int transfer =
                        av_hwframe_transfer_data(software_.get(), working_.get(), 0);
                    transfer < 0) {
                    return toError(transfer, "downloading a hardware frame");
                }
                // The transfer moves pixels but not metadata; timestamps and
                // colour tags live on the original and have to come across.
                av_frame_copy_props(software_.get(), working_.get());
                av_frame_unref(working_.get());
                av_frame_move_ref(working_.get(), software_.get());
            }
            positionPts_ = timestampOf(*working_);
            return {};
        }

        if (rc == AVERROR_EOF) {
            endOfStream_ = true;
            return Error{ErrorCode::EndOfStream, "no more frames"};
        }
        if (rc != AVERROR(EAGAIN)) {
            return toError(rc, "receiving a frame");
        }

        // Decoder wants more input.
        bool sent = false;
        while (!sent) {
            av_packet_unref(packet_.get());
            rc = av_read_frame(format_.get(), packet_.get());
            if (rc == AVERROR_EOF) {
                // Flush: a decoder with reordering still holds frames.
                avcodec_send_packet(codec_.get(), nullptr);
                sent = true;
                break;
            }
            if (rc < 0) {
                return toError(rc, "reading a packet");
            }
            if (packet_->stream_index != streamIndex_) {
                continue;
            }
            if (const int send = avcodec_send_packet(codec_.get(), packet_.get()); send < 0) {
                return toError(send, "sending a packet to the decoder");
            }
            sent = true;
        }
    }
}

Status FFmpegVideoDecoder::seekTo(std::int64_t streamPts) {
    if (const int rc = av_seek_frame(format_.get(), streamIndex_, streamPts, AVSEEK_FLAG_BACKWARD);
        rc < 0) {
        return toError(rc, "seeking");
    }
    avcodec_flush_buffers(codec_.get());
    havePending_ = false;
    endOfStream_ = false;
    positionPts_ = kNoPts;
    return {};
}

Result<media::VideoFrame> FFmpegVideoDecoder::frameAtStreamPts(std::int64_t target) {
    const std::int64_t threshold =
        av_rescale_q(kSeekThresholdMicros, AVRational{1, AV_TIME_BASE}, timeBase_);

    // Decoding forward from where we already are beats a seek for short hops,
    // which is the common case when a playhead is being dragged.
    //
    // The comparison has to be `<=`, not `<`. Frames already consumed cannot be
    // reached again by decoding forward, so a target at the current position is
    // just as unreachable as one behind it -- asking for the same frame twice in
    // a row would otherwise return its successor.
    const bool mustSeek =
        positionPts_ == kNoPts || target <= positionPts_ || (target - positionPts_) > threshold;

    if (mustSeek) {
        if (Status status = seekTo(target); !status) {
            return status.error();
        }
    }

    FramePtr hold = makeFrame();
    if (!hold) {
        return Error{ErrorCode::Internal, "out of memory"};
    }
    bool haveHold = false;

    // Seeking can land too late. MP4 and MOV index their keyframes by decode
    // timestamp, but a stream with B-frames presents frames out of decode
    // order, so the keyframe whose DTS precedes the target may have a PTS after
    // it -- and then decoding forward never reaches the requested frame at all.
    // The fix is to notice we overshot and seek progressively further back.
    const std::int64_t oneSecond = av_rescale_q(1, AVRational{1, 1}, timeBase_);
    std::int64_t backoff = 0;

    for (int attempt = 0; attempt < kMaxSeekAttempts; ++attempt) {
        if (mustSeek || attempt > 0) {
            if (Status status = seekTo(target - backoff); !status) {
                return status.error();
            }
        }

        av_frame_unref(hold.get());
        haveHold = false;

        while (true) {
            if (Status status = pull(); !status) {
                if (status.error().code() == ErrorCode::EndOfStream && haveHold) {
                    break;
                }
                return status.error();
            }

            const std::int64_t pts = timestampOf(*working_);
            if (haveHold && pts > target) {
                // Read one frame too far. Stash it so a following nextFrame()
                // call still sees a continuous stream, and wind the recorded
                // position back to the frame being returned -- the stashed
                // frame has not been consumed, so it is still reachable
                // without a seek.
                av_frame_unref(pending_.get());
                av_frame_move_ref(pending_.get(), working_.get());
                havePending_ = true;
                positionPts_ = timestampOf(*hold);
                break;
            }

            av_frame_unref(hold.get());
            av_frame_move_ref(hold.get(), working_.get());
            haveHold = true;

            if (pts >= target) {
                break;
            }
        }

        if (!haveHold) {
            return Error{ErrorCode::NotFound, "no frame at or before the requested time"};
        }
        if (timestampOf(*hold) <= target) {
            break;
        }
        // Overshot. If there is nothing earlier to seek to, the target simply
        // precedes the first frame and this is the right answer.
        if (target - backoff <= firstPts_) {
            break;
        }
        backoff = backoff == 0 ? oneSecond : backoff * 2;
    }

    auto frame = toVideoFrame(*hold, scaler_);
    if (!frame) {
        return frame.error();
    }
    const std::int64_t heldPts = timestampOf(*hold);
    frame->setPts(time::RationalTime::fromSeconds(
        fromAv(timeBase_) * time::Rational::fromInt(heldPts), info_.frameRate));
    if (conformed_) {
        const auto it = std::lower_bound(conformIndex_.begin(), conformIndex_.end(), heldPts);
        if (it != conformIndex_.end() && *it == heldPts) {
            frame->setSourceIndex(std::distance(conformIndex_.begin(), it));
        }
    }
    return frame;
}

Result<media::VideoFrame> FFmpegVideoDecoder::nextFrame() {
    if (Status status = pull(); !status) {
        return status.error();
    }
    auto frame = toVideoFrame(*working_, scaler_);
    if (!frame) {
        return frame.error();
    }
    const std::int64_t pts = timestampOf(*working_);
    frame->setPts(time::RationalTime::fromSeconds(fromAv(timeBase_) * time::Rational::fromInt(pts),
                                                  info_.frameRate));
    return frame;
}

Result<media::VideoFrame> FFmpegVideoDecoder::stillPicture() {
    if (haveStill_) {
        return still_.clone();
    }
    // Read first, rewind only if that finds nothing.
    //
    // The order matters and is not the obvious one. Seeking first looks safer
    // and breaks `image2`, the demuxer a .jpg usually arrives on: a seek there
    // leaves the read head past the single frame the file holds, and the pull
    // that follows reports end of stream on a picture that decodes perfectly
    // well. A decoder nobody has read from is already in the right place, so
    // the seek is only wanted for the second caller -- and it is allowed to
    // fail, because several still demuxers do not seek at all.
    Status status = pull();
    if (!status && status.error().code() == ErrorCode::EndOfStream) {
        if (Status rewound = seekTo(0); rewound) {
            status = pull();
        }
    }
    if (!status) {
        return status.error();
    }
    auto frame = toVideoFrame(*working_, scaler_);
    if (!frame) {
        return frame.error();
    }
    // Time zero, whatever the container claimed. A still is the picture at
    // every instant it is on screen, and giving it the timestamp it happened to
    // carry would make the frame cache hold it under a key nothing asks for.
    frame->setPts(time::RationalTime{0, info_.frameRate});
    frame->setSourceIndex(0);
    still_ = std::move(*frame);
    haveStill_ = true;
    return still_.clone();
}

Result<media::VideoFrame> FFmpegVideoDecoder::frameAtTime(const time::RationalTime& t) {
    if (info_.isStill) {
        // Every instant of a still is the same picture, so the time asked for
        // is not consulted. This is what lets a photograph be stretched to any
        // length: there is no source to run out of, and no frame to fail to
        // find.
        return stillPicture();
    }
    const time::Rational seconds = t.toSeconds();
    const time::Rational inTimeBase = seconds / fromAv(timeBase_);
    return frameAtStreamPts(inTimeBase.floorToInt());
}

Result<media::VideoFrame> FFmpegVideoDecoder::frameAtIndex(std::int64_t index) {
    if (index < 0) {
        return Error{ErrorCode::NotFound, "negative frame index"};
    }
    if (info_.isStill) {
        if (index > 0) {
            return Error{ErrorCode::NotFound,
                         "a still has one frame; there is no frame " + std::to_string(index)};
        }
        return stillPicture();
    }
    if (!conformed_) {
        if (Status status = conform(); !status) {
            return status.error();
        }
    }
    if (index >= static_cast<std::int64_t>(conformIndex_.size())) {
        return Error{ErrorCode::NotFound, "frame index " + std::to_string(index) +
                                              " is past the end (" +
                                              std::to_string(conformIndex_.size()) + " frames)"};
    }
    auto frame = frameAtStreamPts(conformIndex_[static_cast<std::size_t>(index)]);
    if (frame) {
        frame->setSourceIndex(index);
    }
    return frame;
}

Status FFmpegVideoDecoder::conform() {
    if (conformed_) {
        return {};
    }

    // A still is one frame at time zero, and scanning the file to discover that
    // finds nothing: the image2 and *_pipe demuxers routinely hand over their
    // single packet with neither a pts nor a dts, so the scan below recorded no
    // timestamps and reported "0 frames" -- which is why a .jpg could be probed
    // and then not opened.
    if (info_.isStill) {
        conformIndex_ = {0};
        conformed_ = true;
        return {};
    }

    std::vector<std::int64_t> timestamps;
    if (info_.frameCountHint > 0) {
        timestamps.reserve(static_cast<std::size_t>(info_.frameCountHint));
    }

    if (const int rc = av_seek_frame(format_.get(), streamIndex_, 0, AVSEEK_FLAG_BACKWARD);
        rc < 0) {
        return toError(rc, "rewinding to conform");
    }

    PacketPtr packet = makePacket();
    while (true) {
        av_packet_unref(packet.get());
        const int rc = av_read_frame(format_.get(), packet.get());
        if (rc == AVERROR_EOF) {
            break;
        }
        if (rc < 0) {
            return toError(rc, "scanning packets to conform");
        }
        if (packet->stream_index != streamIndex_) {
            continue;
        }
        const std::int64_t pts = packet->pts != kNoPts ? packet->pts : packet->dts;
        if (pts != kNoPts) {
            timestamps.push_back(pts);
        }
    }

    // Packets arrive in decode order; with B-frames that is not presentation
    // order. Sorting puts them back, and dropping duplicates guards against
    // containers that repeat a timestamp.
    std::sort(timestamps.begin(), timestamps.end());
    timestamps.erase(std::unique(timestamps.begin(), timestamps.end()), timestamps.end());

    conformIndex_ = std::move(timestamps);
    conformed_ = true;

    // The scan moved the read head and invalidated the decoder's state.
    avcodec_flush_buffers(codec_.get());
    havePending_ = false;
    endOfStream_ = false;
    positionPts_ = kNoPts;
    return {};
}

Result<std::int64_t> FFmpegVideoDecoder::frameCount() {
    if (!conformed_) {
        if (Status status = conform(); !status) {
            return status.error();
        }
    }
    return static_cast<std::int64_t>(conformIndex_.size());
}

}  // namespace

Result<std::unique_ptr<media::VideoDecoder>> openVideoDecoder(
    const std::string& path, const media::DecoderOptions& options) {
    return FFmpegVideoDecoder::open(path, options);
}

}  // namespace zaro::platform::ffmpeg
