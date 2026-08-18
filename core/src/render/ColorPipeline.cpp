#include "zaro/core/render/ColorPipeline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <utility>

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

namespace {

/// A sampled transfer curve.
///
/// The scalar versions above call std::pow, which is fine for one value and
/// ruinous for two million pixels times three channels: measured, it was
/// essentially all of the cost of compositing a 1080p frame. The curves are
/// smooth and bounded to [0,1], so a table with linear interpolation is
/// accurate to far better than 8-bit quantisation at the price of an index and
/// a multiply.
class TransferLut {
public:
    static constexpr std::size_t kSteps = 4096;

    TransferLut(TransferFunction transfer, bool toLinearDirection) {
        for (std::size_t i = 0; i <= kSteps; ++i) {
            const auto value = static_cast<float>(i) / static_cast<float>(kSteps);
            table_[i] = toLinearDirection ? toLinearScalar(value, transfer)
                                          : fromLinearScalar(value, transfer);
        }
    }

    [[nodiscard]] float operator()(float value) const noexcept {
        const float scaled = std::clamp(value, 0.0F, 1.0F) * static_cast<float>(kSteps);
        const auto index = static_cast<std::size_t>(scaled);
        if (index >= kSteps) {
            return table_[kSteps];
        }
        const float fraction = scaled - static_cast<float>(index);
        return table_[index] + (table_[index + 1] - table_[index]) * fraction;
    }

private:
    std::array<float, kSteps + 1> table_{};
};

/// One table per curve per direction, built once. There are a handful of curves
/// and they never change.
const TransferLut& lutFor(TransferFunction transfer, bool toLinearDirection) {
    static const std::map<std::pair<int, bool>, TransferLut> kTables = [] {
        std::map<std::pair<int, bool>, TransferLut> tables;
        for (const TransferFunction curve :
             {TransferFunction::Unknown, TransferFunction::BT709, TransferFunction::Gamma22,
              TransferFunction::Gamma28, TransferFunction::SMPTE170M, TransferFunction::SRGB,
              TransferFunction::Linear}) {
            for (const bool direction : {true, false}) {
                tables.emplace(std::pair{static_cast<int>(curve), direction},
                               TransferLut{curve, direction});
            }
        }
        return tables;
    }();
    const auto found = kTables.find({static_cast<int>(transfer), toLinearDirection});
    return found != kTables.end()
               ? found->second
               : kTables.at({static_cast<int>(TransferFunction::BT709), toLinearDirection});
}

}  // namespace

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

    // P010 left-justifies its 10 bits in a 16-bit word; everything else is
    // right-justified, so the deep formats need different normalisation.
    const float deepShift = source.format() == PixelFormat::P010 ? 1.0F / 64.0F : 1.0F;
    const float midpoint = static_cast<float>(1 << (format.bitsPerComponent - 1));
    const TransferLut& lut = lutFor(transfer, true);

    const auto readSample = [deep](const std::uint8_t* row, std::int32_t index) -> float {
        if (!deep) {
            return static_cast<float>(row[index]);
        }
        std::uint16_t value = 0;
        std::memcpy(&value, row + static_cast<std::size_t>(index) * 2, sizeof(value));
        return static_cast<float>(value);
    };

    // Row pointers rather than a per-pixel accessor. sampleAt() carries
    // always-on bounds checks, which are the right thing everywhere except the
    // one place they run six million times a frame.
    for (std::int32_t y = 0; y < height; ++y) {
        const std::int32_t chromaY = y >> format.chromaShiftY;
        const std::uint8_t* lumaRow =
            source.plane(0) +
            static_cast<std::size_t>(y) * static_cast<std::size_t>(source.stride(0));
        const std::uint8_t* cbRow =
            source.plane(1) +
            static_cast<std::size_t>(chromaY) * static_cast<std::size_t>(source.stride(1));
        const std::uint8_t* crRow =
            semiPlanar ? nullptr
                       : source.plane(2) + static_cast<std::size_t>(chromaY) *
                                               static_cast<std::size_t>(source.stride(2));

        Rgba* destination = out.row(y);
        for (std::int32_t x = 0; x < width; ++x) {
            const std::int32_t chromaX = x >> format.chromaShiftX;

            float rawY = readSample(lumaRow, x);
            float rawCb = 0.0F;
            float rawCr = 0.0F;
            if (semiPlanar) {
                // NV12 and P010 interleave Cb and Cr in one plane.
                rawCb = readSample(cbRow, chromaX * 2);
                rawCr = readSample(cbRow, chromaX * 2 + 1);
            } else {
                rawCb = readSample(cbRow, chromaX);
                rawCr = readSample(crRow, chromaX);
            }
            if (deep) {
                rawY *= deepShift;
                rawCb *= deepShift;
                rawCr *= deepShift;
            }

            const float yy = (rawY - scale.lumaOffset) * scale.lumaScale;
            // Chroma is centred on the midpoint of its range, not on zero.
            const float cbCentred = (rawCb - midpoint) * scale.chromaScale;
            const float crCentred = (rawCr - midpoint) * scale.chromaScale;

            const float rPrime = yy + crToR * crCentred;
            const float gPrime = yy - crToG * crCentred - cbToG * cbCentred;
            const float bPrime = yy + cbToB * cbCentred;

            destination[x] = Rgba{lut(rPrime), lut(gPrime), lut(bPrime), 1.0F};
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

    const TransferLut& lut = lutFor(transfer, false);

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
                return static_cast<std::uint8_t>(
                    std::lround(clamp01(lut(clamp01(value * inverse))) * 255.0F));
            };
            out[x * 3 + 0] = encode(pixel.r);
            out[x * 3 + 1] = encode(pixel.g);
            out[x * 3 + 2] = encode(pixel.b);
        }
    }
    return {};
}

}  // namespace zaro::render
