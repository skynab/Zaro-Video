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

/// Apply a clip's effects to its image, in order.
///
/// `scratch` is kept by the caller between frames, because these allocate
/// frame-sized buffers and a render loop that did that per frame would spend
/// more time in the allocator than in the filter.
void applyEffects(const std::vector<model::Effect>& effects, RgbaImage& image, RgbaImage& scratch,
                  RgbaImage& scratch2);

}  // namespace zaro::render
