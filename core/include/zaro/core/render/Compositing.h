#pragma once

#include "zaro/core/model/ClipEffects.h"
#include "zaro/core/render/Grade.h"
#include "zaro/core/render/RgbaImage.h"

namespace zaro::render {

/// Draw `source` into `destination` under `transform`, compositing with
/// `blend`. Both are premultiplied linear RGBA.
///
/// Implemented as an inverse map: for each destination pixel, work out where it
/// came from in the source and sample there. That is what a GPU does, so the
/// shader port produces the same picture, and it avoids the gaps a forward map
/// leaves when scaling up.
/// A grade, if there is one, is applied to each *sampled* colour rather than to
/// the source beforehand. That is what the shader does — it grades what comes
/// back from the texture unit — and the two have to agree. It also means a
/// grade costs nothing for the pixels a transform never samples.
void drawTransformed(const RgbaImage& source, RgbaImage& destination,
                     const model::Transform& transform,
                     model::BlendMode blend = model::BlendMode::Normal,
                     const GradeConstants* grade = nullptr, const CurveTable* curves = nullptr,
                     const SecondaryConstants* secondary = nullptr, const LutTable* lut = nullptr,
                     float lutAmount = 1.0F);

/// Composite `source` over `destination` with no geometry -- the common case,
/// and much faster than going through the sampler.
void drawOver(const RgbaImage& source, RgbaImage& destination, double opacity = 1.0,
              model::BlendMode blend = model::BlendMode::Normal,
              const GradeConstants* grade = nullptr, const CurveTable* curves = nullptr,
              const SecondaryConstants* secondary = nullptr, const LutTable* lut = nullptr,
              float lutAmount = 1.0F);

}  // namespace zaro::render
