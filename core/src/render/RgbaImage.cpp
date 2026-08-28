#include "zaro/core/render/RgbaImage.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "zaro/core/Check.h"

namespace zaro::render {

RgbaImage::RgbaImage(std::int32_t width, std::int32_t height) : width_{width}, height_{height} {
    ZARO_CHECK(width > 0 && height > 0, "RgbaImage with non-positive dimensions");
    pixels_.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), Rgba{});
}

void RgbaImage::clear() {
    std::fill(pixels_.begin(), pixels_.end(), Rgba{});
}

void RgbaImage::fill(const Rgba& colour) {
    std::fill(pixels_.begin(), pixels_.end(), colour);
}

RgbaImage RgbaImage::clone() const {
    RgbaImage copy;
    copy.width_ = width_;
    copy.height_ = height_;
    copy.pixels_ = pixels_;
    return copy;
}

Rgba RgbaImage::sampleBilinear(float x, float y) const {
    // Everything outside is transparent black. Clamping instead would smear the
    // edge pixel outwards, which shows up as a streak along the frame border on
    // anything scaled up or rotated.
    if (!isValid() || x < -1.0F || y < -1.0F || x > static_cast<float>(width_) ||
        y > static_cast<float>(height_)) {
        return Rgba{};
    }

    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const auto x0 = static_cast<std::int32_t>(fx);
    const auto y0 = static_cast<std::int32_t>(fy);
    const float tx = x - fx;
    const float ty = y - fy;

    const auto fetch = [this](std::int32_t px, std::int32_t py) -> Rgba {
        if (px < 0 || py < 0 || px >= width_ || py >= height_) {
            return Rgba{};
        }
        return at(px, py);
    };

    const Rgba p00 = fetch(x0, y0);
    const Rgba p10 = fetch(x0 + 1, y0);
    const Rgba p01 = fetch(x0, y0 + 1);
    const Rgba p11 = fetch(x0 + 1, y0 + 1);

    const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const auto blend = [&](float a00, float a10, float a01, float a11) {
        return lerp(lerp(a00, a10, tx), lerp(a01, a11, tx), ty);
    };

    // Interpolating premultiplied values is why this is correct: colour is
    // already weighted by coverage, so a transparent neighbour contributes
    // nothing rather than dragging its colour in.
    return Rgba{blend(p00.r, p10.r, p01.r, p11.r), blend(p00.g, p10.g, p01.g, p11.g),
                blend(p00.b, p10.b, p01.b, p11.b), blend(p00.a, p10.a, p01.a, p11.a)};
}

}  // namespace zaro::render
