#pragma once

#include "zaro/core/model/Graphic.h"
#include "zaro/core/model/Mask.h"
#include "zaro/core/render/RgbaImage.h"

namespace zaro::render {

/// Draw a generated shape into a frame-sized image.
///
/// The image is cleared first: a graphic clip's picture is the shape and
/// transparency, not the shape over whatever the buffer held.
///
/// Antialiased by sampling coverage rather than by supersampling the whole
/// frame. A shape's edge is the only part that needs it, and computing coverage
/// from the distance to the edge costs nothing away from that edge — where an
/// N× supersample costs N× everywhere.
void drawShape(const model::Graphic& graphic, RgbaImage& out);

/// How much of the pixel at (x, y) the shape covers, 0 to 1.
///
/// Exposed because it is the whole of the shape's geometry, and a test that
/// checks coverage directly is checking the thing that matters rather than
/// inferring it from pixels that have also been through premultiplication.
[[nodiscard]] float shapeCoverage(const model::Graphic& graphic, std::int32_t width,
                                  std::int32_t height, double x, double y);

/// How much of the pixel at (x, y) a mask lets through, 0 to 1.
///
/// The same geometry the shape rasteriser uses, so a mask and a shape of the
/// same size cover exactly the same pixels — which is what anyone would assume
/// on seeing the two controls side by side.
[[nodiscard]] float maskCoverage(const model::Mask& mask, std::int32_t width, std::int32_t height,
                                 double x, double y);

}  // namespace zaro::render
