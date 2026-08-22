#pragma once

#include <vector>

#include "zaro/core/model/Effect.h"
#include "zaro/core/render/RgbaImage.h"

namespace zaro::render {

/// Blur an image in place, with a Gaussian of standard deviation `radius`.
///
/// **Separable, in two passes.** A Gaussian is the product of a horizontal and
/// a vertical one, so a radius that would cost a thousand samples per pixel as
/// a square costs sixty-four as two lines. That is the difference between an
/// effect somebody can drag a slider on and one they set and wait for.
///
/// **On premultiplied values.** Blurring straight colours pulls the colour of
/// transparent pixels into the visible ones -- the black halo that appears
/// around anything blurred over an alpha edge. Premultiplied, a transparent
/// pixel contributes nothing to either sum, which is the whole reason ADR-005
/// picked that representation.
///
/// **In linear light**, like everything else the compositor does: the average
/// of two brightnesses is the brightness of their average only in linear, and a
/// blur is nothing but averages.
void blur(RgbaImage& image, RgbaImage& scratch, float radius);

/// Bend the picture radially about its centre, and scale it while doing so.
///
/// **One radial term, not a lens profile.** `r' = r * (1 + curvature * r^2)`,
/// with r measured in half-diagonals so the corners sit at 1 and the
/// coefficient means the same thing whatever the frame size is. Real lenses
/// need more terms and a decentring pair to model exactly; one term is what
/// straightens the barrel on a wide shot, which is what this is for. A profile
/// per lens is a database, not an effect.
///
/// **Resampled from a copy, once.** Every output pixel reads one bilinear
/// sample of the original, so the distortion is applied once rather than
/// accumulated -- resampling in place would smear each pixel through the ones
/// already moved.
///
/// **Outside the source is transparent, not clamped.** Straightening a barrel
/// means the corners read from beyond the frame edge, where there is nothing;
/// a clamped read would smear the border pixel outwards into a streak instead.
/// Empty corners are honest about what is missing, and `Zoom` is how somebody
/// fills them.
void distort(RgbaImage& image, RgbaImage& scratch, float curvature, float zoom);

/// Apply a clip's effects to its image, in order.
///
/// `scratch` is kept by the caller between frames, because these allocate
/// frame-sized buffers and a render loop that did that per frame would spend
/// more time in the allocator than in the filter.
/// `seconds` is the clip's source time, the same coordinate every other curve
/// on a clip is keyed in (ADR-008), so an effect keyframed against a moment in
/// the footage stays on it through a trim.
void applyEffects(const std::vector<model::Effect>& effects, RgbaImage& image, RgbaImage& scratch,
                  RgbaImage& scratch2, double seconds = 0.0);

}  // namespace zaro::render
