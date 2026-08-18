#pragma once

#include "zaro/core/model/ClipEffects.h"
#include "zaro/core/render/RgbaImage.h"

namespace zaro::render {

/// Draw `source` into `destination` under `transform`, compositing with
/// `blend`. Both are premultiplied linear RGBA.
///
/// Implemented as an inverse map: for each destination pixel, work out where it
/// came from in the source and sample there. That is what a GPU does, so the
/// shader port produces the same picture, and it avoids the gaps a forward map
/// leaves when scaling up.
void drawTransformed(const RgbaImage& source, RgbaImage& destination,
                     const model::Transform& transform,
                     model::BlendMode blend = model::BlendMode::Normal);

/// Composite `source` over `destination` with no geometry -- the common case,
/// and much faster than going through the sampler.
void drawOver(const RgbaImage& source, RgbaImage& destination, double opacity = 1.0,
              model::BlendMode blend = model::BlendMode::Normal);

}  // namespace zaro::render
