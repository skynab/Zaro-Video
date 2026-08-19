#pragma once

#include "zaro/core/model/ColorCorrection.h"
#include "zaro/core/render/CurveTable.h"
#include "zaro/core/render/RgbaImage.h"

namespace zaro::render {

/// The channel gains a white balance comes down to.
///
/// Separated out because the shader needs the same three numbers and must not
/// re-derive them: two implementations of "what does temperature mean" is two
/// answers, and the preview would disagree with the export by an amount too
/// small to notice and too large to accept.
struct WhiteBalanceGains {
    float r{1.0F};
    float g{1.0F};
    float b{1.0F};
};

[[nodiscard]] WhiteBalanceGains whiteBalanceFor(const model::ColorCorrection& correction);

/// Everything the shader needs, precomputed once per clip rather than per
/// pixel: gains, the exposure multiplier, the contrast exponent.
struct GradeConstants {
    WhiteBalanceGains balance;
    float exposure{1.0F};
    float contrast{1.0F};
    float saturation{1.0F};

    [[nodiscard]] bool isIdentity() const noexcept {
        return balance.r == 1.0F && balance.g == 1.0F && balance.b == 1.0F && exposure == 1.0F &&
               contrast == 1.0F && saturation == 1.0F;
    }
};

[[nodiscard]] GradeConstants gradeConstantsFor(const model::ColorCorrection& correction);

/// Middle grey in scene-linear light, and the pivot contrast turns about.
///
/// 0.18, not 0.5. Half is the middle of an *encoded* signal; in linear light it
/// is nearly two stops above middle grey, and pivoting there makes every
/// contrast adjustment darken the picture as a side effect.
inline constexpr float kMiddleGrey = 0.18F;

/// Grade one un-premultiplied linear colour.
///
/// The order is white balance, exposure, contrast, saturation, and it is the
/// photographic one: light is balanced and exposed before a tone curve is
/// applied to it, and saturation last so it acts on the tones being delivered
/// rather than on the ones on the way in. The shader applies the same order;
/// this is the reference it is checked against.
/// The curve table, if any, is applied *after* the primary correction: a curve
/// is drawn against what the picture looks like once it has been balanced and
/// exposed, so applying it first would change its meaning whenever exposure
/// moved.
void gradePixel(const GradeConstants& grade, float& r, float& g, float& b,
                const CurveTable* curves = nullptr);

/// Grade a whole image in place. The image is premultiplied, so alpha is
/// divided out and multiplied back: grading a half-faded clip must not depend
/// on how faded it is.
void gradeImage(const GradeConstants& grade, RgbaImage& image, const CurveTable* curves = nullptr);

}  // namespace zaro::render
