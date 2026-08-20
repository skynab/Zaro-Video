#include "zaro/core/render/Keyer.h"

#include <algorithm>
#include <cmath>

#include "zaro/core/render/ColorPipeline.h"
#include "zaro/core/render/Qualifier.h"

namespace zaro::render {
namespace {

constexpr float kLumaR = 0.2126F;
constexpr float kLumaG = 0.7152F;
constexpr float kLumaB = 0.0722F;

/// Below this total intensity a pixel has no reliable colour: dividing by it
/// turns sensor noise into a hue. Such pixels are kept rather than dropped --
/// black in front of a green screen is a subject, not a hole.
constexpr float kIntensityFloor = 1e-4F;

}  // namespace

KeyerConstants keyerConstantsFor(const model::Keyer& keyer, media::TransferFunction transfer) {
    KeyerConstants out;
    out.kind = keyer.kind;
    if (!keyer.isSet()) {
        return out;
    }
    out.showMatte = keyer.showMatte;
    out.spill = static_cast<float>(std::clamp(keyer.spill, 0.0, 1.0));

    const float red = toLinearScalar(static_cast<float>(std::clamp(keyer.red, 0.0, 1.0)), transfer);
    const float green =
        toLinearScalar(static_cast<float>(std::clamp(keyer.green, 0.0, 1.0)), transfer);
    const float blue =
        toLinearScalar(static_cast<float>(std::clamp(keyer.blue, 0.0, 1.0)), transfer);

    const float sum = red + green + blue;
    if (sum > kIntensityFloor) {
        out.keyR = red / sum;
        out.keyG = green / sum;
        out.keyB = blue / sum;
    } else {
        // A key colour of black has no chromaticity. Rather than divide by
        // nothing, the key becomes one that selects the neutral axis, which is
        // what black is.
        out.keyR = out.keyG = out.keyB = 1.0F / 3.0F;
    }

    out.spillChannel = 1;
    if (red > green && red >= blue) {
        out.spillChannel = 0;
    } else if (blue > green && blue > red) {
        out.spillChannel = 2;
    }

    const auto tolerance = static_cast<float>(std::max(0.0, keyer.tolerance));
    out.tolerance = tolerance;
    out.outer = tolerance + static_cast<float>(std::max(0.0, keyer.softness));

    const double lumaSoft = std::max(0.0, keyer.lumaSoftness);
    const auto toLinear = [transfer](double display) {
        return toLinearScalar(static_cast<float>(std::clamp(display, 0.0, 1.0)), transfer);
    };
    out.lumaInnerLow = toLinear(keyer.lumaLow);
    out.lumaOuterLow = toLinear(keyer.lumaLow - lumaSoft);
    out.lumaInnerHigh = toLinear(keyer.lumaHigh);
    // The same rule the qualifier uses: a window reaching display white has to
    // include everything above it, because linear light does not stop there.
    out.lumaOuterHigh =
        keyer.lumaHigh >= 1.0 ? 1e9F : toLinear(std::min(1.0, keyer.lumaHigh + lumaSoft));
    return out;
}

float keyMatte(const KeyerConstants& keyer, float r, float g, float b) {
    if (!keyer.isActive()) {
        return 1.0F;
    }

    if (keyer.kind == model::KeyKind::Luma) {
        const float luma = (kLumaR * r) + (kLumaG * g) + (kLumaB * b);
        const float inside = rampUp(luma, keyer.lumaOuterLow, keyer.lumaInnerLow) *
                             rampDown(luma, keyer.lumaInnerHigh, keyer.lumaOuterHigh);
        return std::clamp(1.0F - inside, 0.0F, 1.0F);
    }

    const float sum = r + g + b;
    if (sum <= kIntensityFloor) {
        return 1.0F;
    }
    const float dr = (r / sum) - keyer.keyR;
    const float dg = (g / sum) - keyer.keyG;
    const float db = (b / sum) - keyer.keyB;
    const float distance = std::sqrt((dr * dr) + (dg * dg) + (db * db));
    // Inside the tolerance is background and goes to zero; beyond the falloff
    // is foreground and stays.
    return rampUp(distance, keyer.tolerance, keyer.outer);
}

void suppressSpill(const KeyerConstants& keyer, float& r, float& g, float& b) {
    if (!keyer.isActive() || keyer.kind != model::KeyKind::Chroma || keyer.spill <= 0.0F) {
        return;
    }
    float* channels[3] = {&r, &g, &b};
    const std::size_t index = static_cast<std::size_t>(std::clamp(keyer.spillChannel, 0, 2));
    const std::size_t other1 = (index + 1) % 3;
    const std::size_t other2 = (index + 2) % 3;

    const float dominant = *channels[index];
    const float ceiling = (*channels[other1] + *channels[other2]) * 0.5F;
    if (dominant <= ceiling) {
        return;
    }
    *channels[index] = dominant + ((ceiling - dominant) * keyer.spill);
}

}  // namespace zaro::render
