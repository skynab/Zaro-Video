#pragma once

#include <cstdint>

// The single place libav* headers are included. Everything that touches an
// AVFrame or an AVFormatContext lives behind this file.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <memory>
#include <string>

#include "zaro/core/Error.h"
#include "zaro/core/media/ColorInfo.h"
#include "zaro/core/media/MediaInfo.h"
#include "zaro/core/media/PixelFormat.h"
#include "zaro/core/media/VideoFrame.h"
#include "zaro/core/time/Rational.h"

namespace zaro::platform::ffmpeg {

// AV_NOPTS_VALUE expands to a C-style cast, so it is pinned here once rather
// than sprinkled through code compiled with -Wold-style-cast.
inline constexpr std::int64_t kNoPts = AV_NOPTS_VALUE;

/// libav returns negative AVERROR codes; turn one into something a human can
/// act on, preserving the distinction between "ran out" and "went wrong".
[[nodiscard]] Error toError(int averror, const std::string& context);
[[nodiscard]] ErrorCode classify(int averror);

// --- RAII ------------------------------------------------------------------
struct FormatContextDeleter {
    void operator()(AVFormatContext* p) const noexcept { avformat_close_input(&p); }
};
struct CodecContextDeleter {
    void operator()(AVCodecContext* p) const noexcept { avcodec_free_context(&p); }
};
struct FrameDeleter {
    void operator()(AVFrame* p) const noexcept { av_frame_free(&p); }
};
struct PacketDeleter {
    void operator()(AVPacket* p) const noexcept { av_packet_free(&p); }
};
struct BufferRefDeleter {
    void operator()(AVBufferRef* p) const noexcept { av_buffer_unref(&p); }
};
struct SwsDeleter {
    void operator()(SwsContext* p) const noexcept { sws_freeContext(p); }
};
struct SwrDeleter {
    void operator()(SwrContext* p) const noexcept { swr_free(&p); }
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using BufferRefPtr = std::unique_ptr<AVBufferRef, BufferRefDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsDeleter>;
using SwrContextPtr = std::unique_ptr<SwrContext, SwrDeleter>;

[[nodiscard]] FramePtr makeFrame();
[[nodiscard]] PacketPtr makePacket();

// --- Conversions -----------------------------------------------------------
[[nodiscard]] time::Rational fromAv(AVRational r);
[[nodiscard]] AVRational toAv(const time::Rational& r);

[[nodiscard]] media::PixelFormat fromAv(AVPixelFormat format);
[[nodiscard]] AVPixelFormat toAv(media::PixelFormat format);

[[nodiscard]] media::ColorInfo colorInfoFrom(const AVCodecParameters& par);
[[nodiscard]] media::ColorInfo colorInfoFrom(const AVFrame& frame);

/// Copy an AVFrame's pixels into an owning VideoFrame, converting the pixel
/// format only if it is one we do not carry natively.
[[nodiscard]] Result<media::VideoFrame> toVideoFrame(const AVFrame& frame, SwsContextPtr& scaler);

/// Rescale a timestamp between timebases without going through floating point.
[[nodiscard]] std::int64_t rescalePts(std::int64_t pts, AVRational from, AVRational to);

}  // namespace zaro::platform::ffmpeg
