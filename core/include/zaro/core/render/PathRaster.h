#pragma once

#include <vector>

#include "zaro/core/model/MaskPath.h"
#include "zaro/core/render/RgbaImage.h"

namespace zaro::render {

/// A path flattened to straight segments, in output coordinates.
///
/// Kept as a type rather than recomputed per pixel: flattening is the expensive
/// part and it depends on nothing but the path, while the coverage question is
/// asked a million times a frame.
using FlatPath = std::vector<std::pair<double, double>>;

/// Turn a path's cubic segments into a polyline.
///
/// Subdivided by how far each segment strays from its chord, not by a fixed
/// count. A fixed count is either wasteful on a nearly straight segment or
/// visibly faceted on a tight one, and a mask's edge is exactly where facets
/// show.
[[nodiscard]] FlatPath flatten(const model::MaskPath& path, double tolerance = 0.25);

/// Rasterise a closed polyline into a coverage buffer, one float per pixel.
///
/// **Scanline, with the nonzero winding rule.** Even-odd would make a path that
/// crosses itself punch a hole in its own middle, which is a surprise every
/// time; nonzero treats the crossing as still inside, which is what somebody
/// dragging a point through their own outline expects to see.
///
/// Coverage is antialiased vertically by sampling several rows per pixel and
/// horizontally by the exact span the crossings give, so an edge at any angle
/// comes out smooth. The alternative -- a hard fill blurred afterwards -- moves
/// the edge by half the blur, which is visible on a mask somebody has lined up
/// against something.
void rasterisePath(const FlatPath& path, std::int32_t width, std::int32_t height,
                   std::vector<float>& coverage);

/// The coverage of a whole mask path, feathered, as a frame-sized buffer.
///
/// Feather is a blur of the coverage. It is separable and it is the same blur
/// the effect stack uses, which is why a feathered path and a feathered
/// rectangle soften at the same rate.
void rasteriseMaskPath(const model::MaskPath& path, double feather, bool inverted,
                       std::int32_t width, std::int32_t height, std::vector<float>& coverage);

}  // namespace zaro::render
