#include <cstdint>
#include <cstdio>
#include <vector>

#include "zaro/platform/ffmpeg/FFmpegMedia.h"

#include "Ffi.h"

namespace zaro::platform::ffmpeg {
namespace {

int swsColorspaceFor(media::ColorMatrix matrix) {
    switch (matrix) {
        case media::ColorMatrix::BT709:
            return SWS_CS_ITU709;
        case media::ColorMatrix::BT601:
            return SWS_CS_ITU601;
        case media::ColorMatrix::BT2020NCL:
            return SWS_CS_BT2020;
        case media::ColorMatrix::SMPTE240M:
            return SWS_CS_SMPTE240M;
        default:
            return SWS_CS_DEFAULT;
    }
}

/// Wrap a VideoFrame's planes in an AVFrame without copying them.
void describe(const media::VideoFrame& source, AVFrame& out) {
    out.width = source.width();
    out.height = source.height();
    out.format = toAv(source.format());
    for (std::size_t i = 0; i < source.planeCount(); ++i) {
        out.data[i] = const_cast<std::uint8_t*>(source.plane(i));
        out.linesize[i] = source.stride(i);
    }
}

}  // namespace

Status writePng(const media::VideoFrame& source, const std::string& path) {
    if (!source.isValid()) {
        return Error{ErrorCode::InvalidData, "cannot write an invalid frame"};
    }

    FramePtr input = makeFrame();
    FramePtr rgb = makeFrame();
    if (!input || !rgb) {
        return Error{ErrorCode::Internal, "out of memory"};
    }
    describe(source, *input);

    rgb->width = source.width();
    rgb->height = source.height();
    rgb->format = AV_PIX_FMT_RGB24;
    if (const int rc = av_frame_get_buffer(rgb.get(), 32); rc < 0) {
        return toError(rc, "allocating an RGB frame");
    }

    SwsContextPtr scaler{sws_getContext(
        source.width(), source.height(), static_cast<AVPixelFormat>(input->format), source.width(),
        source.height(), AV_PIX_FMT_RGB24, SWS_BICUBIC, nullptr, nullptr, nullptr)};
    if (!scaler) {
        return Error{ErrorCode::Unsupported, "no RGB conversion for this pixel format"};
    }

    // Honour the frame's own tags. Converting BT.601 material with BT.709
    // coefficients, or limited-range as full, is the difference between a
    // correct still and a subtly wrong one that survives review.
    const media::ColorInfo& color = source.color();
    const int srcRange = color.range == media::ColorRange::Full ? 1 : 0;
    sws_setColorspaceDetails(scaler.get(), sws_getCoefficients(swsColorspaceFor(color.matrix)),
                             srcRange, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);

    if (const int rc = sws_scale(scaler.get(), input->data, input->linesize, 0, source.height(),
                                 rgb->data, rgb->linesize);
        rc < 0) {
        return toError(rc, "converting to RGB");
    }

    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (encoder == nullptr) {
        return Error{ErrorCode::Unsupported, "this FFmpeg build has no PNG encoder"};
    }
    CodecContextPtr codec{avcodec_alloc_context3(encoder)};
    if (!codec) {
        return Error{ErrorCode::Internal, "out of memory"};
    }
    codec->width = source.width();
    codec->height = source.height();
    codec->pix_fmt = AV_PIX_FMT_RGB24;
    codec->time_base = AVRational{1, 25};
    if (const int rc = avcodec_open2(codec.get(), encoder, nullptr); rc < 0) {
        return toError(rc, "opening the PNG encoder");
    }
    if (const int rc = avcodec_send_frame(codec.get(), rgb.get()); rc < 0) {
        return toError(rc, "encoding a PNG");
    }
    avcodec_send_frame(codec.get(), nullptr);

    PacketPtr packet = makePacket();
    if (const int rc = avcodec_receive_packet(codec.get(), packet.get()); rc < 0) {
        return toError(rc, "receiving the encoded PNG");
    }

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return Error{ErrorCode::Io, "cannot open " + path + " for writing"};
    }
    const std::size_t written =
        std::fwrite(packet->data, 1, static_cast<std::size_t>(packet->size), file);
    std::fclose(file);
    if (written != static_cast<std::size_t>(packet->size)) {
        return Error{ErrorCode::Io, "short write to " + path};
    }
    return {};
}

Status writeRawPlanes(const media::VideoFrame& frame, const std::string& path) {
    if (!frame.isValid()) {
        return Error{ErrorCode::InvalidData, "cannot write an invalid frame"};
    }
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return Error{ErrorCode::Io, "cannot open " + path + " for writing"};
    }

    for (std::size_t plane = 0; plane < frame.planeCount(); ++plane) {
        const auto index = static_cast<std::int32_t>(plane);
        const auto rows =
            static_cast<std::size_t>(media::planeHeight(frame.format(), frame.height(), index));
        const auto bytes =
            static_cast<std::size_t>(media::rowBytes(frame.format(), frame.width(), index));
        for (std::size_t row = 0; row < rows; ++row) {
            const std::uint8_t* start =
                frame.plane(plane) + row * static_cast<std::size_t>(frame.stride(plane));
            if (std::fwrite(start, 1, bytes, file) != bytes) {
                std::fclose(file);
                return Error{ErrorCode::Io, "short write to " + path};
            }
        }
    }
    std::fclose(file);
    return {};
}

}  // namespace zaro::platform::ffmpeg
