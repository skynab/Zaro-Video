#include "zaro/core/render/Qualifier.h"

#include <algorithm>
#include <cmath>

#include "zaro/core/render/ColorPipeline.h"

namespace zaro::render {
namespace {

constexpr float kLumaR = 0.2126F;
constexpr float kLumaG = 0.7152F;
constexpr float kLumaB = 0.0722F;

}  // namespace

float smoothly(float t) {
    const float clamped = std::clamp(t, 0.0F, 1.0F);
    return clamped * clamped * (3.0F - (2.0F * clamped));
}

/// 0 below `outer`, 1 above `inner`, smooth between. `outer <= inner`.
float rampUp(float value, float outer, float inner) {
    if (inner <= outer) {
        return value >= inner ? 1.0F : 0.0F;
    }
    return smoothly((value - outer) / (inner - outer));
}

/// 1 below `inner`, 0 above `outer`, smooth between. `inner <= outer`.
float rampDown(float value, float inner, float outer) {
    if (outer <= inner) {
        return value <= inner ? 1.0F : 0.0F;
    }
    return smoothly((outer - value) / (outer - inner));
}

namespace {

/// The shorter way round the hue circle, in degrees.
///
/// Hue wraps, and a window centred on red runs from about 350 to about 10. A
/// plain subtraction says those are 340 degrees apart and selects nothing,
/// which is exactly the selection someone reaching for a skin tone wants.
float hueDistance(float from, float to) {
    float difference = std::fabs(from - to);
    if (difference > 180.0F) {
        difference = 360.0F - difference;
    }
    return difference;
}

}  // namespace

QualifierConstants qualifierConstantsFor(const model::HslQualifier& qualifier,
                                         media::TransferFunction transfer) {
    QualifierConstants out;
    out.enabled = qualifier.enabled;
    if (!qualifier.enabled) {
        return out;
    }

    const auto halfWidth = static_cast<float>(std::clamp(qualifier.hueWidth, 0.0, 360.0) / 2.0);
    out.hueCentre = static_cast<float>(std::fmod(qualifier.hueCentre, 360.0));
    if (out.hueCentre < 0.0F) {
        out.hueCentre += 360.0F;
    }
    out.hueInner = halfWidth;
    out.hueOuter = halfWidth + static_cast<float>(std::max(0.0, qualifier.hueSoftness));

    const auto satSoft = static_cast<float>(std::max(0.0, qualifier.saturationSoftness));
    out.satInnerLow = static_cast<float>(qualifier.saturationLow);
    out.satOuterLow = out.satInnerLow - satSoft;
    out.satInnerHigh = static_cast<float>(qualifier.saturationHigh);
    out.satOuterHigh = out.satInnerHigh + satSoft;

    // The thresholds are display-referred; the pixels they will be compared
    // against are linear. Converting here means the comparison downstream is a
    // subtraction, with no transfer function anywhere near a per-pixel path.
    const double lumaSoft = std::max(0.0, qualifier.lumaSoftness);
    const auto toLinear = [transfer](double display) {
        return toLinearScalar(static_cast<float>(std::clamp(display, 0.0, 1.0)), transfer);
    };
    out.lumaInnerLow = toLinear(qualifier.lumaLow);
    out.lumaOuterLow = toLinear(qualifier.lumaLow - lumaSoft);
    out.lumaInnerHigh = toLinear(qualifier.lumaHigh);
    // The top of the range is special: a selection reaching display white has
    // to keep everything above it too, because linear light does not stop at
    // white and a highlight is not "outside the highlights".
    out.lumaOuterHigh =
        qualifier.lumaHigh >= 1.0 ? 1e9F : toLinear(std::min(1.0, qualifier.lumaHigh + lumaSoft));
    return out;
}

void hueSaturationOf(float r, float g, float b, float& hue, float& saturation) {
    const float high = std::max({r, g, b});
    const float low = std::min({r, g, b});
    const float range = high - low;

    saturation = high > 0.0001F ? range / high : 0.0F;
    if (range <= 0.0001F) {
        hue = 0.0F;  // neutral has no hue; any answer is arbitrary, so pick one
        return;
    }

    float degrees = 0.0F;
    if (high == r) {
        degrees = 60.0F * std::fmod((g - b) / range, 6.0F);
    } else if (high == g) {
        degrees = 60.0F * (((b - r) / range) + 2.0F);
    } else {
        degrees = 60.0F * (((r - g) / range) + 4.0F);
    }
    hue = degrees < 0.0F ? degrees + 360.0F : degrees;
}

float qualifierMask(const QualifierConstants& qualifier, float r, float g, float b) {
    if (!qualifier.enabled) {
        return 1.0F;
    }

    float hue = 0.0F;
    float saturation = 0.0F;
    hueSaturationOf(r, g, b, hue, saturation);
    const float luma = (kLumaR * r) + (kLumaG * g) + (kLumaB * b);

    // A window covering the whole circle selects every hue, including the
    // neutral pixels whose hue is arbitrary. Without this, "everything dark"
    // would quietly drop the greys, which are most of what is dark.
    const float hueMask = qualifier.hueInner >= 180.0F
                              ? 1.0F
                              : rampDown(hueDistance(hue, qualifier.hueCentre), qualifier.hueInner,
                                         qualifier.hueOuter);

    const float satMask = rampUp(saturation, qualifier.satOuterLow, qualifier.satInnerLow) *
                          rampDown(saturation, qualifier.satInnerHigh, qualifier.satOuterHigh);

    const float lumaMask = rampUp(luma, qualifier.lumaOuterLow, qualifier.lumaInnerLow) *
                           rampDown(luma, qualifier.lumaInnerHigh, qualifier.lumaOuterHigh);

    return std::clamp(hueMask * satMask * lumaMask, 0.0F, 1.0F);
}

}  // namespace zaro::render
