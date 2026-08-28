#include "zaro/core/render/Scopes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "zaro/core/Check.h"
#include "zaro/core/render/ColorPipeline.h"

namespace zaro::render {
namespace {

/// Rec.709 luma. The same coefficients the vectorscope's colour difference
/// axes are built from, so the two instruments agree about what neutral is.
constexpr float kLumaR = 0.2126F;
constexpr float kLumaG = 0.7152F;
constexpr float kLumaB = 0.0722F;

/// Cb and Cr scaling, so a fully saturated primary reaches the edge of the
/// plot rather than half of it.
constexpr float kCbScale = 1.0F / 1.8556F;
constexpr float kCrScale = 1.0F / 1.5748F;

/// The transfer curve, sampled once.
///
/// The encode is a `pow` per channel per pixel, and at 1080p that is six
/// million of them per frame — measured, in an earlier phase, as essentially
/// the entire cost of a frame. A 1024-entry table costs a lookup and a lerp,
/// and the error is far below the quantisation of the 256 levels being
/// measured into.
class TransferTable {
public:
    explicit TransferTable(media::TransferFunction transfer) {
        for (std::size_t i = 0; i < kEntries; ++i) {
            const auto linear = static_cast<float>(i) / static_cast<float>(kEntries - 1);
            table_[i] = fromLinearScalar(linear, transfer);
        }
    }

    [[nodiscard]] float encode(float linear) const {
        // Out of range is clipped, not extrapolated: this is measuring what a
        // display will show, and a display shows black and white.
        if (!(linear > 0.0F)) {
            return 0.0F;  // also catches NaN, which must not become an index
        }
        if (linear >= 1.0F) {
            return 1.0F;
        }
        const float scaled = linear * static_cast<float>(kEntries - 1);
        const auto index = static_cast<std::size_t>(scaled);
        const float fraction = scaled - static_cast<float>(index);
        return table_[index] + ((table_[index + 1] - table_[index]) * fraction);
    }

private:
    static constexpr std::size_t kEntries = 1024;
    std::array<float, kEntries + 1> table_{};
};

std::int32_t toLevel(float encoded) {
    const auto level = static_cast<std::int32_t>(encoded * (Waveform::kLevels - 1) + 0.5F);
    return std::clamp(level, 0, Waveform::kLevels - 1);
}

}  // namespace

Waveform::Waveform(std::int32_t columns, ScopeChannel channel)
    : columns_{std::max(0, columns)}, channel_{channel} {
    cells_.assign(static_cast<std::size_t>(columns_) * kLevels, 0U);
}

std::uint32_t Waveform::at(std::int32_t column, std::int32_t level) const {
    ZARO_CHECK(column >= 0 && column < columns_, "waveform column out of range");
    ZARO_CHECK(level >= 0 && level < kLevels, "waveform level out of range");
    return cells_[(static_cast<std::size_t>(column) * kLevels) + static_cast<std::size_t>(level)];
}

void Waveform::add(std::int32_t column, std::int32_t level) {
    if (column < 0 || column >= columns_ || level < 0 || level >= kLevels) {
        return;
    }
    const std::size_t index =
        (static_cast<std::size_t>(column) * kLevels) + static_cast<std::size_t>(level);
    peak_ = std::max(peak_, ++cells_[index]);
}

Vectorscope::Vectorscope(std::int32_t size) : size_{std::max(0, size)} {
    cells_.assign(static_cast<std::size_t>(size_) * static_cast<std::size_t>(size_), 0U);
}

std::uint32_t Vectorscope::at(std::int32_t x, std::int32_t y) const {
    ZARO_CHECK(x >= 0 && x < size_ && y >= 0 && y < size_, "vectorscope point out of range");
    return cells_[(static_cast<std::size_t>(y) * static_cast<std::size_t>(size_)) +
                  static_cast<std::size_t>(x)];
}

void Vectorscope::add(std::int32_t x, std::int32_t y) {
    if (x < 0 || x >= size_ || y < 0 || y >= size_) {
        return;
    }
    const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(size_)) +
                              static_cast<std::size_t>(x);
    peak_ = std::max(peak_, ++cells_[index]);
}

void Vectorscope::plotFor(float r, float g, float b, std::int32_t size, float& x, float& y) {
    const float luma = (kLumaR * r) + (kLumaG * g) + (kLumaB * b);
    const float cb = (b - luma) * kCbScale;
    const float cr = (r - luma) * kCrScale;
    const float half = static_cast<float>(size) * 0.5F;
    // Cr upwards, so the plot matches every graticule ever printed: on a
    // vectorscope red is up and to the right, not down.
    x = half + (cb * half);
    y = half - (cr * half);
}

FrameScopes measure(const RgbaImage& frame, const ScopeOptions& options) {
    FrameScopes out;
    const std::int32_t columns = std::max(1, options.waveformColumns);
    out.luma = Waveform{columns, ScopeChannel::Luma};
    out.red = Waveform{columns, ScopeChannel::Red};
    out.green = Waveform{columns, ScopeChannel::Green};
    out.blue = Waveform{columns, ScopeChannel::Blue};
    out.histogram.red.assign(Histogram::kBins, 0U);
    out.histogram.green.assign(Histogram::kBins, 0U);
    out.histogram.blue.assign(Histogram::kBins, 0U);
    out.histogram.luma.assign(Histogram::kBins, 0U);
    out.vectorscope = Vectorscope{std::max(1, options.vectorscopeSize)};
    if (!frame.isValid()) {
        return out;
    }

    const TransferTable transfer{options.transfer};
    const std::int32_t stride = std::max(1, options.rowStride);
    const auto scale = static_cast<double>(columns) / static_cast<double>(frame.width());

    for (std::int32_t y = 0; y < frame.height(); y += stride) {
        const Rgba* row = frame.row(y);
        for (std::int32_t x = 0; x < frame.width(); ++x) {
            const Rgba& pixel = row[x];
            // Un-premultiply before measuring. The working space is
            // premultiplied, so a half-transparent white pixel is stored as
            // 0.5 and would read as mid grey — the scope would report a fade
            // as a change in exposure.
            const float alpha = pixel.a;
            const float inverse = alpha > 0.0001F ? 1.0F / alpha : 0.0F;
            const float r = transfer.encode(pixel.r * inverse);
            const float g = transfer.encode(pixel.g * inverse);
            const float b = transfer.encode(pixel.b * inverse);
            const float luma = (kLumaR * r) + (kLumaG * g) + (kLumaB * b);

            const auto column = std::clamp(static_cast<std::int32_t>(x * scale), 0, columns - 1);
            const std::int32_t levelR = toLevel(r);
            const std::int32_t levelG = toLevel(g);
            const std::int32_t levelB = toLevel(b);
            const std::int32_t levelY = toLevel(luma);

            out.red.add(column, levelR);
            out.green.add(column, levelG);
            out.blue.add(column, levelB);
            out.luma.add(column, levelY);

            ++out.histogram.red[static_cast<std::size_t>(levelR)];
            ++out.histogram.green[static_cast<std::size_t>(levelG)];
            ++out.histogram.blue[static_cast<std::size_t>(levelB)];
            ++out.histogram.luma[static_cast<std::size_t>(levelY)];

            float px = 0.0F;
            float py = 0.0F;
            Vectorscope::plotFor(r, g, b, out.vectorscope.size(), px, py);
            out.vectorscope.add(static_cast<std::int32_t>(px), static_cast<std::int32_t>(py));
        }
    }

    for (const auto* channel :
         {&out.histogram.red, &out.histogram.green, &out.histogram.blue, &out.histogram.luma}) {
        out.histogram.peak =
            std::max(out.histogram.peak, *std::max_element(channel->begin(), channel->end()));
    }
    return out;
}

}  // namespace zaro::render
