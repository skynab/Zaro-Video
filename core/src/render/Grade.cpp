#include "zaro/core/render/Grade.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

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

GradeConstants gradeConstantsFor(const model::ColorCorrection& correction,
                                 const model::ColorWheels& wheels) {
    GradeConstants grade;
    grade.balance = whiteBalanceFor(correction);
    grade.exposure = std::pow(2.0F, static_cast<float>(correction.exposure));
    // Contrast as an exponent about middle grey. A slider at +100 doubles the
    // exponent; at -100 it halves it, so the two ends are inverses of each
    // other and the control is symmetrical in the way it looks.
    const auto amount = static_cast<float>(std::clamp(correction.contrast, -100.0, 100.0) / 100.0);
    grade.contrast = amount >= 0.0F ? 1.0F + amount : 1.0F / (1.0F - amount);
    grade.saturation = static_cast<float>(std::max(0.0, correction.saturation) / 100.0);

    grade.wheels = !wheels.isIdentity();
    const double slopes[3] = {wheels.slopeR, wheels.slopeG, wheels.slopeB};
    const double offsets[3] = {wheels.offsetR, wheels.offsetG, wheels.offsetB};
    const double powers[3] = {wheels.powerR, wheels.powerG, wheels.powerB};
    for (std::size_t i = 0; i < 3; ++i) {
        grade.slope[i] = static_cast<float>(slopes[i]);
        grade.offset[i] = static_cast<float>(offsets[i]);
        // A power of zero or less is not a curve, it is a division by nothing.
        // Clamped here so the per-pixel path never has to check.
        grade.power[i] = static_cast<float>(std::max(1e-3, powers[i]));
    }
    return grade;
}

SecondaryConstants secondaryConstantsFor(const model::Secondary& secondary,
                                         media::TransferFunction transfer) {
    SecondaryConstants out;
    out.qualifier = qualifierConstantsFor(secondary.qualifier, transfer);
    out.grade = gradeConstantsFor(secondary.correction);
    out.showMask = secondary.showMask;
    return out;
}

/// A pixel's hue as a turn around the circle, 0 to 1.
///
/// The same formula the shader uses, in the same place in the chain, because
/// the two are compared pixel for pixel. A neutral pixel has no hue at all --
/// the formula divides by a range of zero -- and answers 0, which is harmless:
/// a pixel with no colour in it is unchanged by any saturation multiplier.
float hueTurn(float r, float g, float b) {
    const float high = std::max(r, std::max(g, b));
    const float low = std::min(r, std::min(g, b));
    const float range = high - low;
    if (range <= 0.0F) {
        return 0.0F;
    }
    float degrees = 0.0F;
    if (high == r) {
        degrees = 60.0F * std::fmod((g - b) / range, 6.0F);
    } else if (high == g) {
        degrees = 60.0F * (((b - r) / range) + 2.0F);
    } else {
        degrees = 60.0F * (((r - g) / range) + 4.0F);
    }
    if (degrees < 0.0F) {
        degrees += 360.0F;
    }
    return degrees / 360.0F;
}

/// Rebuild a pixel at a different hue, keeping how bright and how colourful it
/// is.
///
/// The standard HSV reconstruction, and the exact inverse of `hueTurn` above:
/// `high` is V, `high - low` is the chroma, and `low` is what is added back to
/// every channel. Written out rather than routed through a general HSV
/// conversion so that the shader can hold the same six lines beside it -- the
/// two are compared pixel for pixel, and a shared abstraction neither of them
/// can share is worth less than a duplicate they can both be read against.
///
/// Values above one survive it: V is whatever the brightest channel was, so a
/// highlight at 4.0 comes back at 4.0 in its new hue rather than clipped.
void rebuildAtHue(float turn, float high, float low, float& r, float& g, float& b) {
    const float chroma = high - low;
    const float sector = turn * 6.0F;
    const float rising = chroma * (1.0F - std::fabs(std::fmod(sector, 2.0F) - 1.0F));
    float rr = 0.0F;
    float gg = 0.0F;
    float bb = 0.0F;
    if (sector < 1.0F) {
        rr = chroma;
        gg = rising;
    } else if (sector < 2.0F) {
        rr = rising;
        gg = chroma;
    } else if (sector < 3.0F) {
        gg = chroma;
        bb = rising;
    } else if (sector < 4.0F) {
        gg = rising;
        bb = chroma;
    } else if (sector < 5.0F) {
        rr = rising;
        bb = chroma;
    } else {
        rr = chroma;
        bb = rising;
    }
    r = rr + low;
    g = gg + low;
    b = bb + low;
}

void gradePixel(const GradeConstants& grade, float& r, float& g, float& b, const CurveTable* curves,
                const SecondaryConstants* secondary, const LutTable* lut, float lutAmount,
                const ColorCurveTable* hue) {
    r *= grade.balance.r * grade.exposure;
    g *= grade.balance.g * grade.exposure;
    b *= grade.balance.b * grade.exposure;

    if (grade.wheels) {
        // The CDL, before contrast. Both shape the midtones, and doing the
        // wheels first means the contrast control pivots about middle grey of
        // the picture somebody is actually looking at -- which is what they
        // expect from the control they reached for last.
        //
        // Negative light has no fractional power, and one NaN spreads through
        // everything it touches, so anything at or below zero stops at zero.
        const auto cdl = [](float value, float slope, float offset, float power) {
            const float scaled = (value * slope) + offset;
            return scaled > 0.0F ? std::pow(scaled, power) : 0.0F;
        };
        r = cdl(r, grade.slope[0], grade.offset[0], grade.power[0]);
        g = cdl(g, grade.slope[1], grade.offset[1], grade.power[1]);
        b = cdl(b, grade.slope[2], grade.offset[2], grade.power[2]);
    }

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

    // Saturation, and the hue curve with it, because they are the same
    // operation: one number scales how far a pixel is from grey, and the curve
    // makes that number depend on which way from grey it is. Applying them
    // separately would mix toward luma twice and cost a second pass over the
    // pixel to reach the same place.
    const bool shapedByHue = hue != nullptr && !hue->isIdentity();
    // The hue the pixel arrived with, read once and used by every curve that
    // selects on it. Both the shift and the saturation-against-hue curve pick
    // their colour by what it *was*: shifting the blues and pulling the blues
    // down have to act on the same pixels, not on wherever the first one left
    // them.
    const float arrivedAt = shapedByHue ? hueTurn(r, g, b) : 0.0F;
    if (shapedByHue && hue->hasShift()) {
        const float high = std::max(r, std::max(g, b));
        const float low = std::min(r, std::min(g, b));
        if (high - low > 0.0F) {
            rebuildAtHue(hue->shiftedHue(arrivedAt), high, low, r, g, b);
        }
    }
    if (grade.saturation != 1.0F || shapedByHue) {
        // Toward the luma of the pixel, so desaturating never changes its
        // brightness. Mixing toward a fixed grey would darken saturated
        // colours and lighten dark ones.
        const float grey = luma(r, g, b);
        float amount = grade.saturation;
        if (shapedByHue) {
            // Both curves multiply into the one factor, so a pixel that is both
            // a blue and a shadow gets both adjustments compounded. Two
            // sequential lerps toward luma would not compound -- the second
            // would work on what the first left, which is a different picture
            // and a surprising one.
            if (hue->hasHue()) {
                amount *= hue->saturationAt(arrivedAt);
            }
            if (hue->hasLuma()) {
                amount *= hue->saturationAtLuma(grey);
            }
        }
        r = grey + ((r - grey) * amount);
        g = grey + ((g - grey) * amount);
        b = grey + ((b - grey) * amount);
    }

    if (lut != nullptr && lut->isValid()) {
        lut->apply(r, g, b, lutAmount);
    }

    if (curves != nullptr && !curves->isIdentity()) {
        r = curves->apply(r, 0);
        g = curves->apply(g, 1);
        b = curves->apply(b, 2);
    }

    if (secondary != nullptr && secondary->isActive()) {
        const float mask = qualifierMask(secondary->qualifier, r, g, b);
        if (secondary->showMask) {
            // The mask itself, as a grey picture. Judging a qualifier by
            // looking at the corrected result is guesswork.
            r = mask;
            g = mask;
            b = mask;
            return;
        }
        if (mask > 0.0F) {
            float sr = r;
            float sg = g;
            float sb = b;
            gradePixel(secondary->grade, sr, sg, sb);
            // Blended by the mask rather than switched on it: a soft edge is
            // the whole point of the qualifier, and a hard swap would throw it
            // away at the last step.
            r += (sr - r) * mask;
            g += (sg - g) * mask;
            b += (sb - b) * mask;
        }
    }
}

void gradeImage(const GradeConstants& grade, RgbaImage& image, const CurveTable* curves,
                const SecondaryConstants* secondary, const LutTable* lut, float lutAmount,
                const ColorCurveTable* hue) {
    const bool curved = curves != nullptr && !curves->isIdentity();
    const bool keyed = secondary != nullptr && secondary->isActive();
    const bool looked = lut != nullptr && lut->isValid() && lutAmount > 0.0F;
    const bool shapedByHue = hue != nullptr && !hue->isIdentity();
    if ((grade.isIdentity() && !curved && !keyed && !looked && !shapedByHue) || !image.isValid()) {
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
            gradePixel(grade, r, g, b, curves, secondary, lut, lutAmount, hue);
            pixel.r = r * alpha;
            pixel.g = g * alpha;
            pixel.b = b * alpha;
        }
    }
}

}  // namespace zaro::render
