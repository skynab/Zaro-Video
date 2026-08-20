#include "zaro/core/render/Compositing.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "zaro/core/render/ShapeRaster.h"

namespace zaro::render {
namespace {

using model::BlendMode;
using model::Transform;

/// Blend one premultiplied source pixel onto one premultiplied destination.
Rgba blendPixel(const Rgba& source, const Rgba& destination, BlendMode mode) {
    const float inverseAlpha = 1.0F - source.a;
    switch (mode) {
        case BlendMode::Add:
            return Rgba{source.r + destination.r, source.g + destination.g,
                        source.b + destination.b,
                        std::min(1.0F, source.a + destination.a * inverseAlpha)};
        case BlendMode::Multiply:
            return Rgba{source.r * destination.r + destination.r * inverseAlpha,
                        source.g * destination.g + destination.g * inverseAlpha,
                        source.b * destination.b + destination.b * inverseAlpha,
                        source.a + destination.a * inverseAlpha};
        case BlendMode::Screen:
            return Rgba{source.r + destination.r - source.r * destination.r,
                        source.g + destination.g - source.g * destination.g,
                        source.b + destination.b - source.b * destination.b,
                        source.a + destination.a * inverseAlpha};
        case BlendMode::Normal:
        default:
            // Porter-Duff `over`. On premultiplied values this is a single
            // multiply-add per channel, which is the whole reason for
            // premultiplying.
            return Rgba{
                source.r + destination.r * inverseAlpha, source.g + destination.g * inverseAlpha,
                source.b + destination.b * inverseAlpha, source.a + destination.a * inverseAlpha};
    }
}

Rgba scaled(const Rgba& pixel, float factor) {
    return Rgba{pixel.r * factor, pixel.g * factor, pixel.b * factor, pixel.a * factor};
}

/// Everything one clip does to one pixel: key it, then grade it.
///
/// Un-premultiplied throughout, and multiplied back at the end. Grading the
/// premultiplied values directly would make the correction depend on how
/// transparent the pixel is, so a clip would grade differently in the middle of
/// a dissolve than either side of it -- and a key would find a different colour
/// there too.
///
/// The key runs first, and on the ungraded colour. A key is a measurement of
/// what the camera saw; running it after a grade would mean every adjustment to
/// the look silently moved the edges of the matte.
Rgba shaded(const Rgba& pixel, const GradeConstants* grade, const CurveTable* curves,
            const SecondaryConstants* secondary, const LutTable* lut, float lutAmount,
            const KeyerConstants* keyer) {
    if (pixel.a <= 0.0001F) {
        return pixel;
    }
    const float inverse = 1.0F / pixel.a;
    float r = pixel.r * inverse;
    float g = pixel.g * inverse;
    float b = pixel.b * inverse;

    float alpha = pixel.a;
    if (keyer != nullptr && keyer->isActive()) {
        const float matte = keyMatte(*keyer, r, g, b);
        if (keyer->showMatte) {
            // The matte itself, opaque, so a hole in it is visible against
            // whatever is underneath rather than being one.
            return Rgba{matte * pixel.a, matte * pixel.a, matte * pixel.a, pixel.a};
        }
        suppressSpill(*keyer, r, g, b);
        alpha *= matte;
        if (alpha <= 0.0001F) {
            return Rgba{0.0F, 0.0F, 0.0F, 0.0F};
        }
    }

    if (grade != nullptr) {
        gradePixel(*grade, r, g, b, curves, secondary, lut, lutAmount);
    }
    return Rgba{r * alpha, g * alpha, b * alpha, alpha};
}

}  // namespace

void drawOver(const RgbaImage& source, RgbaImage& destination, double opacity, BlendMode blend,
              const GradeConstants* grade, const CurveTable* curves,
              const SecondaryConstants* secondary, const LutTable* lut, float lutAmount,
              const model::Mask* mask, const KeyerConstants* keyer) {
    if (!source.isValid() || !destination.isValid()) {
        return;
    }
    const auto alpha = static_cast<float>(std::clamp(opacity, 0.0, 1.0));
    if (alpha <= 0.0F) {
        return;
    }

    const std::int32_t width = std::min(source.width(), destination.width());
    const std::int32_t height = std::min(source.height(), destination.height());

    for (std::int32_t y = 0; y < height; ++y) {
        const Rgba* in = source.row(y);
        Rgba* out = destination.row(y);
        for (std::int32_t x = 0; x < width; ++x) {
            // Opacity scales a premultiplied pixel uniformly, colour and
            // coverage together; that is what keeps a fade linear.
            const bool keying = keyer != nullptr && keyer->isActive();
            const Rgba corrected =
                grade != nullptr || keying
                    ? shaded(in[x], grade, curves, secondary, lut, lutAmount, keyer)
                    : in[x];
            // Applied here rather than to the source: a mask is in output
            // coordinates, because where a clip shows through is a fact about
            // the screen and not about the picture being drawn.
            float coverage = alpha;
            if (mask != nullptr && mask->isSet()) {
                coverage *= maskCoverage(*mask, destination.width(), destination.height(), x, y);
            }
            out[x] = blendPixel(coverage == 1.0F ? corrected : scaled(corrected, coverage), out[x],
                                blend);
        }
    }
}

void drawTransformed(const RgbaImage& source, RgbaImage& destination, const Transform& transform,
                     BlendMode blend, const GradeConstants* grade, const CurveTable* curves,
                     const SecondaryConstants* secondary, const LutTable* lut, float lutAmount,
                     const model::Mask* mask, const KeyerConstants* keyer) {
    if (!source.isValid() || !destination.isValid()) {
        return;
    }
    if (transform.isIdentity() && source.width() == destination.width() &&
        source.height() == destination.height()) {
        drawOver(source, destination, 1.0, blend, grade, curves, secondary, lut, lutAmount, mask,
                 keyer);
        return;
    }

    const auto opacity = static_cast<float>(std::clamp(transform.opacity, 0.0, 1.0));
    if (opacity <= 0.0F) {
        return;
    }
    if (transform.scaleX == 0.0 || transform.scaleY == 0.0) {
        return;
    }

    const double radians = transform.rotationDegrees * std::acos(-1.0) / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);

    const double sourceCentreX = source.width() * 0.5;
    const double sourceCentreY = source.height() * 0.5;
    const double destinationCentreX = destination.width() * 0.5;
    const double destinationCentreY = destination.height() * 0.5;

    // Forward transform is: translate by -anchor, scale, rotate, translate by
    // position. This is its inverse, applied per destination pixel.
    const double inverseScaleX = 1.0 / transform.scaleX;
    const double inverseScaleY = 1.0 / transform.scaleY;

    for (std::int32_t y = 0; y < destination.height(); ++y) {
        Rgba* out = destination.row(y);
        for (std::int32_t x = 0; x < destination.width(); ++x) {
            // Sample at pixel centres so a 1:1 transform is an exact copy
            // rather than a half-pixel blur.
            const double dx = (x + 0.5) - destinationCentreX - transform.positionX;
            const double dy = (y + 0.5) - destinationCentreY - transform.positionY;

            const double unrotatedX = dx * cosine + dy * sine;
            const double unrotatedY = -dx * sine + dy * cosine;

            const double sourceX = unrotatedX * inverseScaleX + transform.anchorX + sourceCentreX;
            const double sourceY = unrotatedY * inverseScaleY + transform.anchorY + sourceCentreY;

            Rgba sample = source.sampleBilinear(static_cast<float>(sourceX - 0.5),
                                                static_cast<float>(sourceY - 0.5));
            if (sample.a <= 0.0F && sample.r == 0.0F && sample.g == 0.0F && sample.b == 0.0F) {
                continue;
            }
            if (grade != nullptr || (keyer != nullptr && keyer->isActive())) {
                // Keyed after the sample rather than before it, which is what
                // the shader does -- it keys what comes back from the texture
                // unit. Keying first would mean a scaled clip was keyed at a
                // different resolution than it is drawn at, and the two paths
                // would disagree along every edge.
                sample = shaded(sample, grade, curves, secondary, lut, lutAmount, keyer);
            }
            float coverage = opacity;
            if (mask != nullptr && mask->isSet()) {
                coverage *= maskCoverage(*mask, destination.width(), destination.height(), x, y);
            }
            if (coverage <= 0.0F) {
                continue;
            }
            out[x] =
                blendPixel(coverage == 1.0F ? sample : scaled(sample, coverage), out[x], blend);
        }
    }
}

}  // namespace zaro::render
