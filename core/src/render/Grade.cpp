#include "zaro/core/render/Grade.h"

#include <algorithm>
#include <cmath>

namespace zaro::render {
namespace {

/// Rec.709 luma, the same weights the scopes and the saturation control use.
constexpr float kLumaR = 0.2126F;
constexpr float kLumaG = 0.7152F;
constexpr float kLumaB = 0.0722F;

/// How much a full-scale temperature or tint moves a channel.
///
/// A tuning constant, not a physical one. It is deliberately mild: a slider at
/// its end should be a strong correction, not an unusable one, and a control
/// whose useful range is its first tenth is a control nobody can aim.
constexpr float kBalanceRange = 0.3F;

float luma(float r, float g, float b) {
    return (kLumaR * r) + (kLumaG * g) + (kLumaB * b);
}

}  // namespace

WhiteBalanceGains whiteBalanceFor(const model::ColorCorrection& correction) {
    const auto temperature = static_cast<float>(correction.temperature / 100.0);
    const auto tint = static_cast<float>(correction.tint / 100.0);

    // Warmer means more red and less blue; magenta means less green. Both are
    // channel gains rather than a hue rotation, because a gain is what a
    // different illuminant actually does to a sensor.
    WhiteBalanceGains gains;
    gains.r = 1.0F + (temperature * kBalanceRange);
    gains.b = 1.0F - (temperature * kBalanceRange);
    gains.g = 1.0F - (tint * kBalanceRange);

    // Normalised so white stays at the same brightness. Without this the
    // temperature slider is also an exposure slider, and every correction has
    // to be undone with a second one.
    const float weight = luma(gains.r, gains.g, gains.b);
    if (weight > 0.0001F) {
        gains.r /= weight;
        gains.g /= weight;
        gains.b /= weight;
    }
    return gains;
}

GradeConstants gradeConstantsFor(const model::ColorCorrection& correction) {
    GradeConstants grade;
    grade.balance = whiteBalanceFor(correction);
    grade.exposure = std::pow(2.0F, static_cast<float>(correction.exposure));
    // Contrast as an exponent about middle grey. A slider at +100 doubles the
    // exponent; at -100 it halves it, so the two ends are inverses of each
    // other and the control is symmetrical in the way it looks.
    const auto amount = static_cast<float>(std::clamp(correction.contrast, -100.0, 100.0) / 100.0);
    grade.contrast = amount >= 0.0F ? 1.0F + amount : 1.0F / (1.0F - amount);
    grade.saturation = static_cast<float>(std::max(0.0, correction.saturation) / 100.0);
    return grade;
}

void gradePixel(const GradeConstants& grade, float& r, float& g, float& b) {
    r *= grade.balance.r * grade.exposure;
    g *= grade.balance.g * grade.exposure;
    b *= grade.balance.b * grade.exposure;

    if (grade.contrast != 1.0F) {
        // A power about middle grey. Negative light has no meaning and a
        // fractional power of it has no value at all, so anything at or below
        // zero is left where it is rather than turned into a NaN that spreads
        // through everything it touches.
        const auto pivot = [&](float value) {
            return value > 0.0F ? kMiddleGrey * std::pow(value / kMiddleGrey, grade.contrast)
                                : value;
        };
        r = pivot(r);
        g = pivot(g);
        b = pivot(b);
    }

    if (grade.saturation != 1.0F) {
        // Toward the luma of the pixel, so desaturating never changes its
        // brightness. Mixing toward a fixed grey would darken saturated
        // colours and lighten dark ones.
        const float grey = luma(r, g, b);
        r = grey + ((r - grey) * grade.saturation);
        g = grey + ((g - grey) * grade.saturation);
        b = grey + ((b - grey) * grade.saturation);
    }
}

void gradeImage(const GradeConstants& grade, RgbaImage& image) {
    if (grade.isIdentity() || !image.isValid()) {
        return;
    }
    for (std::int32_t y = 0; y < image.height(); ++y) {
        Rgba* row = image.row(y);
        for (std::int32_t x = 0; x < image.width(); ++x) {
            Rgba& pixel = row[x];
            const float alpha = pixel.a;
            if (alpha <= 0.0001F) {
                continue;  // nothing there to grade, and dividing by it invents colour
            }
            const float inverse = 1.0F / alpha;
            float r = pixel.r * inverse;
            float g = pixel.g * inverse;
            float b = pixel.b * inverse;
            gradePixel(grade, r, g, b);
            pixel.r = r * alpha;
            pixel.g = g * alpha;
            pixel.b = b * alpha;
        }
    }
}

}  // namespace zaro::render
