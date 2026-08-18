#include "zaro/core/render/ColorPipeline.h"

#include <algorithm>
#include <cmath>

namespace zaro::render {
namespace {

using media::ColorMatrix;
using media::ColorRange;
using media::PixelFormat;
using media::TransferFunction;

/// Luma coefficients. Getting these wrong does not break an image, it tints it,
/// which is why the wrong ones survive so long unnoticed.
struct LumaCoefficients {
    float kr;
    float kb;
};

LumaCoefficients coefficientsFor(ColorMatrix matrix) {
    switch (matrix) {
        case ColorMatrix::BT601:
            return {0.299F, 0.114F};
        case ColorMatrix::BT2020NCL:
            return {0.2627F, 0.0593F};
        case ColorMatrix::SMPTE240M:
            return {0.212F, 0.087F};
        case ColorMatrix::BT709:
        default:
            return {0.2126F, 0.0722F};
    }
}

struct YuvScale {
    float lumaOffset;
    float lumaScale;
    float chromaScale;
};

/// Limited range puts luma in 16-235 and chroma in 16-240 of an 8-bit range,
/// scaled up for deeper formats. Treating limited as full crushes blacks and
/// clips highlights, which is the single most common colour bug in video.
YuvScale scaleFor(ColorRange range, int bitDepth) {
    const float peak = static_cast<float>((1 << bitDepth) - 1);
    if (range == ColorRange::Full) {
        return {0.0F, 1.0F / peak, 1.0F / peak};
    }
    const float scale = static_cast<float>(1 << (bitDepth - 8));
    return {16.0F * scale, 1.0F / (219.0F * scale), 1.0F / (224.0F * scale)};
}

float clamp01(float value) {
    return std::clamp(value, 0.0F, 1.0F);
}

}  // namespace

float toLinearScalar(float encoded, TransferFunction transfer) {
    switch (transfer) {
        case TransferFunction::Linear:
            return encoded;
        case TransferFunction::SRGB:
            return encoded <= 0.04045F ? encoded / 12.92F
                                       : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
        case TransferFunction::Gamma22:
            return std::pow(std::max(encoded, 0.0F), 2.2F);
        case TransferFunction::Gamma28:
            return std::pow(std::max(encoded, 0.0F), 2.8F);
        case TransferFunction::BT709:
        case TransferFunction::SMPTE170M:
        default:
            // The inverse of the BT.709 camera OETF. Displays follow BT.1886's
            // gamma 2.4 instead; that is a separate, explicit step and does not
            // belong conflated with the tag's own meaning. See docs/adr/0005.
            return encoded < 0.081F ? encoded / 4.5F
                                    : std::pow((encoded + 0.099F) / 1.099F, 1.0F / 0.45F);
    }
}

float fromLinearScalar(float linear, TransferFunction transfer) {
    switch (transfer) {
        case TransferFunction::Linear:
            return linear;
        case TransferFunction::SRGB:
            return linear <= 0.0031308F ? linear * 12.92F
                                        : 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
        case TransferFunction::Gamma22:
            return std::pow(std::max(linear, 0.0F), 1.0F / 2.2F);
        case TransferFunction::Gamma28:
            return std::pow(std::max(linear, 0.0F), 1.0F / 2.8F);
        case TransferFunction::BT709:
        case TransferFunction::SMPTE170M:
        default:
            return linear < 0.018F ? linear * 4.5F : 1.099F * std::pow(linear, 0.45F) - 0.099F;
    }
}

bool isSupported(const media::ColorInfo& color) {
    return color.transfer != TransferFunction::PQ && color.transfer != TransferFunction::HLG;
}

Status toLinear(const media::VideoFrame& source, RgbaImage& out) {
    if (!source.isValid()) {
        return Error{ErrorCode::InvalidData, "cannot convert an invalid frame"};
    }
    if (!isSupported(source.color())) {
        return Error{ErrorCode::Unsupported,
                     std::string{"HDR transfer function '"} + toString(source.color().transfer) +
                         "' is not handled yet; tone mapping is a later phase"};
    }

    const media::PixelFormatInfo& format = media::info(source.format());
    if (!format.isPlanarYuv) {
        return Error{ErrorCode::Unsupported, std::string{"pixel format "} +
                                                 media::toString(source.format()) +
                                                 " is not a planar or semi-planar Y'CbCr layout"};
    }

    const std::int32_t width = source.width();
    const std::int32_t height = source.height();
    if (out.width() != width || out.height() != height) {
        out = RgbaImage{width, height};
    }

    const LumaCoefficients luma = coefficientsFor(source.color().matrix);
    const float kg = 1.0F - luma.kr - luma.kb;
    const float crToR = 2.0F * (1.0F - luma.kr);
    const float cbToB = 2.0F * (1.0F - luma.kb);
    const float crToG = crToR * luma.kr / kg;
    const float cbToG = cbToB * luma.kb / kg;

    const YuvScale scale = scaleFor(source.color().range, format.bitsPerComponent);
    const TransferFunction transfer = source.color().transfer;
    const bool semiPlanar = format.planeCount == 2;
    const bool deep = format.bitsPerComponent > 8;

    const auto sampleLuma = [&](std::int32_t x, std::int32_t y) -> float {
        return static_cast<float>(source.sampleAt(x, y, 0));
    };
    const auto sampleChroma = [&](std::int32_t x, std::int32_t y, int which) -> float {
        const std::int32_t cx = x >> format.chromaShiftX;
        const std::int32_t cy = y >> format.chromaShiftY;
        if (semiPlanar) {
            // NV12 and P010 interleave Cb and Cr in one plane.
            return static_cast<float>(source.sampleAt(cx * 2 + which, cy, 1));
        }
        return static_cast<float>(source.sampleAt(cx, cy, static_cast<std::size_t>(1 + which)));
    };

    // P010 left-justifies its 10 bits in a 16-bit word; everything else is
    // right-justified, so the deep formats need different normalisation.
    const float deepShift = source.format() == PixelFormat::P010 ? 1.0F / 64.0F : 1.0F;

    for (std::int32_t y = 0; y < height; ++y) {
        Rgba* destination = out.row(y);
        for (std::int32_t x = 0; x < width; ++x) {
            float rawY = sampleLuma(x, y);
            float rawCb = sampleChroma(x, y, 0);
            float rawCr = sampleChroma(x, y, 1);
            if (deep) {
                rawY *= deepShift;
                rawCb *= deepShift;
                rawCr *= deepShift;
            }

            const float yy = (rawY - scale.lumaOffset) * scale.lumaScale;
            // Chroma is centred on the midpoint of its range, not on zero.
            const float midpoint = static_cast<float>(1 << (format.bitsPerComponent - 1));
            const float cbCentred = (rawCb - midpoint) * scale.chromaScale;
            const float crCentred = (rawCr - midpoint) * scale.chromaScale;

            const float rPrime = yy + crToR * crCentred;
            const float gPrime = yy - crToG * crCentred - cbToG * cbCentred;
            const float bPrime = yy + cbToB * cbCentred;

            destination[x] = Rgba{toLinearScalar(clamp01(rPrime), transfer),
                                  toLinearScalar(clamp01(gPrime), transfer),
                                  toLinearScalar(clamp01(bPrime), transfer), 1.0F};
        }
    }
    return {};
}

Status toDisplayRgb24(const RgbaImage& source, std::uint8_t* destination, std::int32_t strideBytes,
                      TransferFunction transfer) {
    if (!source.isValid()) {
        return Error{ErrorCode::InvalidData, "cannot encode an invalid image"};
    }
    if (destination == nullptr || strideBytes < source.width() * 3) {
        return Error{ErrorCode::InvalidData, "destination buffer is too small"};
    }

    for (std::int32_t y = 0; y < source.height(); ++y) {
        const Rgba* pixels = source.row(y);
        std::uint8_t* out =
            destination + static_cast<std::size_t>(y) * static_cast<std::size_t>(strideBytes);
        for (std::int32_t x = 0; x < source.width(); ++x) {
            const Rgba& pixel = pixels[x];
            // Un-premultiply against the composited alpha. Fully transparent
            // pixels have no colour to recover, so they become black rather
            // than a division by zero.
            const float alpha = pixel.a;
            const float inverse = alpha > 1e-6F ? 1.0F / alpha : 0.0F;

            const auto encode = [&](float value) {
                return static_cast<std::uint8_t>(std::lround(
                    clamp01(fromLinearScalar(clamp01(value * inverse), transfer)) * 255.0F));
            };
            out[x * 3 + 0] = encode(pixel.r);
            out[x * 3 + 1] = encode(pixel.g);
            out[x * 3 + 2] = encode(pixel.b);
        }
    }
    return {};
}

}  // namespace zaro::render
