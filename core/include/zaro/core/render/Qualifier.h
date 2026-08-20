#pragma once

#include "zaro/core/media/VideoFrame.h"
#include "zaro/core/model/Secondary.h"

namespace zaro::render {

/// A qualifier reduced to the numbers a per-pixel test needs.
///
/// Windows are stored as their inner and outer edges rather than as centre and
/// softness, so the test is two comparisons and a divide rather than a
/// reconstruction of the same thing at every pixel. The shader receives exactly
/// this, which is what keeps the two implementations from disagreeing about
/// what "softness 15" means.
struct QualifierConstants {
    /// Half-width of the hue window and where its falloff ends, in degrees.
    float hueCentre{0.0F};
    float hueInner{180.0F};
    float hueOuter{180.0F};

    float satInnerLow{0.0F};
    float satOuterLow{0.0F};
    float satInnerHigh{1.0F};
    float satOuterHigh{1.0F};

    /// Linear, converted from the display-referred thresholds the user set.
    float lumaInnerLow{0.0F};
    float lumaOuterLow{0.0F};
    float lumaInnerHigh{1e9F};
    float lumaOuterHigh{1e9F};

    /// False when the qualifier selects everything, so the whole test can be
    /// skipped rather than computed and found to be 1.
    bool enabled{false};
};

[[nodiscard]] QualifierConstants qualifierConstantsFor(const model::HslQualifier& qualifier,
                                                       media::TransferFunction transfer);

/// Smoothstep, the shape every soft edge in the renderer uses.
///
/// One shape everywhere means a mask has no seam where one axis takes over
/// from another, and a key's edge looks like a qualifier's.
[[nodiscard]] float smoothly(float t);
/// 0 below `outer`, 1 above `inner`, smooth between. `outer <= inner`.
[[nodiscard]] float rampUp(float value, float outer, float inner);
/// 1 below `inner`, 0 above `outer`, smooth between. `inner <= outer`.
[[nodiscard]] float rampDown(float value, float inner, float outer);

/// Hue in degrees and saturation in 0..1, from linear RGB.
///
/// Scene-referred, deliberately: hue and saturation of light are properties of
/// the light, and computing them from an encoded signal would make a
/// qualifier's meaning depend on the display curve the sequence happens to
/// carry. Luma is the one axis where a display-referred threshold is what
/// somebody means, and that one is converted at the edge instead.
void hueSaturationOf(float r, float g, float b, float& hue, float& saturation);

/// How much this pixel is selected, 0 to 1.
[[nodiscard]] float qualifierMask(const QualifierConstants& qualifier, float r, float g, float b);

}  // namespace zaro::render
