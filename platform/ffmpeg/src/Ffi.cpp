#include "Ffi.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace zaro::platform::ffmpeg {

ErrorCode classify(int averror) {
    if (averror == AVERROR_EOF) {
        return ErrorCode::EndOfStream;
    }
    if (averror == AVERROR(ENOENT)) {
        return ErrorCode::NotFound;
    }
    if (averror == AVERROR_DECODER_NOT_FOUND || averror == AVERROR_DEMUXER_NOT_FOUND ||
        averror == AVERROR_PROTOCOL_NOT_FOUND || averror == AVERROR_STREAM_NOT_FOUND) {
        return ErrorCode::Unsupported;
    }
    if (averror == AVERROR_INVALIDDATA) {
        return ErrorCode::InvalidData;
    }
    if (averror == AVERROR(EIO)) {
        return ErrorCode::Io;
    }
    return ErrorCode::DecodeFailed;
}

Error toError(int averror, const std::string& context) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(averror, buffer.data(), buffer.size());
    return Error{classify(averror), context + " (" + buffer.data() + ")"};
}

FramePtr makeFrame() {
    return FramePtr{av_frame_alloc()};
}
PacketPtr makePacket() {
    return PacketPtr{av_packet_alloc()};
}

time::Rational fromAv(AVRational r) {
    if (r.den == 0) {
        return time::Rational{0, 1};
    }
    return time::Rational{r.num, r.den};
}

AVRational toAv(const time::Rational& r) {
    // AVRational is a pair of ints, so a rational whose terms exceed int range
    // cannot be represented. Reduce toward the nearest representable value
    // rather than truncating the numerator and silently changing the rate.
    if (r.num() <= INT32_MAX && r.den() <= INT32_MAX && r.num() >= INT32_MIN) {
        return AVRational{static_cast<int>(r.num()), static_cast<int>(r.den())};
    }
    AVRational out{};
    av_reduce(&out.num, &out.den, r.num(), r.den(), INT32_MAX);
    return out;
}

std::int64_t rescalePts(std::int64_t pts, AVRational from, AVRational to) {
    if (pts == kNoPts) {
        return kNoPts;
    }
    return av_rescale_q(pts, from, to);
}

media::PixelFormat fromAv(AVPixelFormat format) {
    switch (format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
            return media::PixelFormat::YUV420P;
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUVJ422P:
            return media::PixelFormat::YUV422P;
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_YUVJ444P:
            return media::PixelFormat::YUV444P;
        case AV_PIX_FMT_YUV420P10LE:
            return media::PixelFormat::YUV420P10;
        case AV_PIX_FMT_YUV422P10LE:
            return media::PixelFormat::YUV422P10;
        case AV_PIX_FMT_YUV444P10LE:
            return media::PixelFormat::YUV444P10;
        case AV_PIX_FMT_NV12:
            return media::PixelFormat::NV12;
        case AV_PIX_FMT_P010LE:
            return media::PixelFormat::P010;
        case AV_PIX_FMT_RGB24:
            return media::PixelFormat::RGB24;
        case AV_PIX_FMT_RGBA:
            return media::PixelFormat::RGBA8;
        default:
            return media::PixelFormat::Unknown;
    }
}

AVPixelFormat toAv(media::PixelFormat format) {
    switch (format) {
        case media::PixelFormat::YUV420P:
            return AV_PIX_FMT_YUV420P;
        case media::PixelFormat::YUV422P:
            return AV_PIX_FMT_YUV422P;
        case media::PixelFormat::YUV444P:
            return AV_PIX_FMT_YUV444P;
        case media::PixelFormat::YUV420P10:
            return AV_PIX_FMT_YUV420P10LE;
        case media::PixelFormat::YUV422P10:
            return AV_PIX_FMT_YUV422P10LE;
        case media::PixelFormat::YUV444P10:
            return AV_PIX_FMT_YUV444P10LE;
        case media::PixelFormat::NV12:
            return AV_PIX_FMT_NV12;
        case media::PixelFormat::P010:
            return AV_PIX_FMT_P010LE;
        case media::PixelFormat::RGB24:
            return AV_PIX_FMT_RGB24;
        case media::PixelFormat::RGBA8:
            return AV_PIX_FMT_RGBA;
        default:
            return AV_PIX_FMT_NONE;
    }
}

namespace {

media::ColorPrimaries primariesFrom(AVColorPrimaries v) {
    switch (v) {
        case AVCOL_PRI_BT709:
            return media::ColorPrimaries::BT709;
        case AVCOL_PRI_SMPTE170M:
            return media::ColorPrimaries::BT601_525;
        case AVCOL_PRI_BT470BG:
            return media::ColorPrimaries::BT601_625;
        case AVCOL_PRI_BT2020:
            return media::ColorPrimaries::BT2020;
        case AVCOL_PRI_SMPTE432:
            return media::ColorPrimaries::DisplayP3;
        default:
            return media::ColorPrimaries::Unknown;
    }
}

media::TransferFunction transferFrom(AVColorTransferCharacteristic v) {
    switch (v) {
        case AVCOL_TRC_BT709:
            return media::TransferFunction::BT709;
        case AVCOL_TRC_GAMMA22:
            return media::TransferFunction::Gamma22;
        case AVCOL_TRC_GAMMA28:
            return media::TransferFunction::Gamma28;
        case AVCOL_TRC_SMPTE170M:
            return media::TransferFunction::SMPTE170M;
        case AVCOL_TRC_IEC61966_2_1:
            return media::TransferFunction::SRGB;
        case AVCOL_TRC_LINEAR:
            return media::TransferFunction::Linear;
        case AVCOL_TRC_SMPTE2084:
            return media::TransferFunction::PQ;
        case AVCOL_TRC_ARIB_STD_B67:
            return media::TransferFunction::HLG;
        default:
            return media::TransferFunction::Unknown;
    }
}

media::ColorMatrix matrixFrom(AVColorSpace v) {
    switch (v) {
        case AVCOL_SPC_BT709:
            return media::ColorMatrix::BT709;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            return media::ColorMatrix::BT601;
        case AVCOL_SPC_BT2020_NCL:
            return media::ColorMatrix::BT2020NCL;
        case AVCOL_SPC_SMPTE240M:
            return media::ColorMatrix::SMPTE240M;
        case AVCOL_SPC_RGB:
            return media::ColorMatrix::Identity;
        default:
            return media::ColorMatrix::Unknown;
    }
}

media::ColorRange rangeFrom(AVColorRange v) {
    switch (v) {
        case AVCOL_RANGE_MPEG:
            return media::ColorRange::Limited;
        case AVCOL_RANGE_JPEG:
            return media::ColorRange::Full;
        default:
            return media::ColorRange::Unknown;
    }
}

}  // namespace

media::ColorInfo colorInfoFrom(const AVCodecParameters& par) {
    return media::ColorInfo{primariesFrom(par.color_primaries), transferFrom(par.color_trc),
                            matrixFrom(par.color_space), rangeFrom(par.color_range)};
}

media::ColorInfo colorInfoFrom(const AVFrame& frame) {
    return media::ColorInfo{primariesFrom(frame.color_primaries), transferFrom(frame.color_trc),
                            matrixFrom(frame.colorspace), rangeFrom(frame.color_range)};
}

Result<media::VideoFrame> toVideoFrame(const AVFrame& source, SwsContextPtr& scaler) {
    const auto sourceFormat = static_cast<AVPixelFormat>(source.format);
    media::PixelFormat target = fromAv(sourceFormat);

    const AVFrame* input = &source;
    FramePtr converted;

    if (target == media::PixelFormat::Unknown) {
        // Something outside the set we carry natively. Convert once, here, to a
        // layout of matching bit depth so nothing downstream needs to know that
        // this format exists.
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(sourceFormat);
        const bool deep = desc != nullptr && desc->comp[0].depth > 8;
        target = deep ? media::PixelFormat::YUV420P10 : media::PixelFormat::YUV420P;

        const AVPixelFormat targetAv = toAv(target);
        scaler.reset(sws_getCachedContext(scaler.release(), source.width, source.height,
                                          sourceFormat, source.width, source.height, targetAv,
                                          SWS_BICUBIC, nullptr, nullptr, nullptr));
        if (!scaler) {
            return Error{ErrorCode::Unsupported, std::string{"no conversion from pixel format "} +
                                                     av_get_pix_fmt_name(sourceFormat)};
        }
        converted = makeFrame();
        if (!converted) {
            return Error{ErrorCode::Internal, "out of memory allocating a conversion frame"};
        }
        converted->format = targetAv;
        converted->width = source.width;
        converted->height = source.height;
        if (const int rc = av_frame_get_buffer(converted.get(), 32); rc < 0) {
            return toError(rc, "allocating a conversion frame");
        }
        if (const int rc = sws_scale(scaler.get(), source.data, source.linesize, 0, source.height,
                                     converted->data, converted->linesize);
            rc < 0) {
            return toError(rc, "converting pixel format");
        }
        input = converted.get();
    }

    media::VideoFrame out = media::VideoFrame::allocate(input->width, input->height, target);
    const media::PixelFormatInfo& fmt = media::info(target);

    for (std::size_t plane = 0; plane < fmt.planeCount; ++plane) {
        const auto index = static_cast<std::int32_t>(plane);
        const auto rows = static_cast<std::size_t>(media::planeHeight(target, out.height(), index));
        const auto bytes = static_cast<std::size_t>(media::rowBytes(target, out.width(), index));
        const auto sourceStride = static_cast<std::size_t>(input->linesize[plane]);
        const auto targetStride = static_cast<std::size_t>(out.stride(plane));
        for (std::size_t row = 0; row < rows; ++row) {
            std::memcpy(out.plane(plane) + row * targetStride,
                        input->data[plane] + row * sourceStride, bytes);
        }
    }

    out.setColor(colorInfoFrom(*input).resolved(out.width(), out.height()));
    out.setKeyframe(source.flags & AV_FRAME_FLAG_KEY);
    if (source.sample_aspect_ratio.num > 0) {
        out.setPixelAspect(fromAv(source.sample_aspect_ratio));
    }
    return out;
}

}  // namespace zaro::platform::ffmpeg
