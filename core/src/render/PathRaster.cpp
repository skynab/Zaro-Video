#include "zaro/core/render/PathRaster.h"

#include <algorithm>
#include <cmath>

#include "zaro/core/render/EffectStack.h"

namespace zaro::render {
namespace {

/// How far a cubic strays from the straight line between its ends.
///
/// The control points are what pull it away, so the distance from each to the
/// chord bounds the whole curve. Cheap, and never under-estimates -- which is
/// the direction that matters, because under-estimating leaves visible facets.
double flatnessOf(double x0, double y0, double cx1, double cy1, double cx2, double cy2, double x1,
                  double y1) {
    const double dx = x1 - x0;
    const double dy = y1 - y0;
    const double length = std::sqrt((dx * dx) + (dy * dy));
    if (length < 1e-9) {
        return std::max(std::hypot(cx1 - x0, cy1 - y0), std::hypot(cx2 - x0, cy2 - y0));
    }
    const auto distance = [&](double px, double py) {
        return std::fabs(((px - x0) * dy) - ((py - y0) * dx)) / length;
    };
    return std::max(distance(cx1, cy1), distance(cx2, cy2));
}

void subdivide(FlatPath& out, double x0, double y0, double cx1, double cy1, double cx2, double cy2,
               double x1, double y1, double tolerance, int depth) {
    // A depth limit as well as a flatness one: a degenerate curve -- control
    // points on top of each other, or NaN from a corrupt file -- would
    // otherwise never satisfy the tolerance and would subdivide until the
    // stack ran out.
    if (depth >= 16 || flatnessOf(x0, y0, cx1, cy1, cx2, cy2, x1, y1) <= tolerance) {
        out.emplace_back(x1, y1);
        return;
    }
    const double x01 = (x0 + cx1) / 2.0;
    const double y01 = (y0 + cy1) / 2.0;
    const double x12 = (cx1 + cx2) / 2.0;
    const double y12 = (cy1 + cy2) / 2.0;
    const double x23 = (cx2 + x1) / 2.0;
    const double y23 = (cy2 + y1) / 2.0;
    const double x012 = (x01 + x12) / 2.0;
    const double y012 = (y01 + y12) / 2.0;
    const double x123 = (x12 + x23) / 2.0;
    const double y123 = (y12 + y23) / 2.0;
    const double mx = (x012 + x123) / 2.0;
    const double my = (y012 + y123) / 2.0;
    subdivide(out, x0, y0, x01, y01, x012, y012, mx, my, tolerance, depth + 1);
    subdivide(out, mx, my, x123, y123, x23, y23, x1, y1, tolerance, depth + 1);
}

}  // namespace

FlatPath flatten(const model::MaskPath& path, double tolerance) {
    FlatPath out;
    if (!path.isSet()) {
        return out;
    }
    const std::size_t count = path.points.size();
    out.emplace_back(path.points.front().x, path.points.front().y);
    for (std::size_t i = 0; i < count; ++i) {
        const model::MaskPoint& from = path.points[i];
        const model::MaskPoint& to = path.points[(i + 1) % count];
        subdivide(out, from.x, from.y, from.x + from.outX, from.y + from.outY, to.x + to.inX,
                  to.y + to.inY, to.x, to.y, std::max(tolerance, 1e-3), 0);
    }
    return out;
}

void rasterisePath(const FlatPath& path, std::int32_t width, std::int32_t height,
                   std::vector<float>& coverage) {
    coverage.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0.0F);
    if (path.size() < 3 || width <= 0 || height <= 0) {
        return;
    }

    // Four sample rows per pixel. Enough that a near-horizontal edge does not
    // stair-step, cheap enough that a full-frame mask is not the slowest thing
    // in the renderer.
    constexpr int kRows = 4;
    const auto halfWidth = static_cast<double>(width) / 2.0;
    const auto halfHeight = static_cast<double>(height) / 2.0;

    struct Crossing {
        double x;
        int direction;
    };
    std::vector<Crossing> crossings;
    std::vector<float> row(static_cast<std::size_t>(width), 0.0F);

    for (std::int32_t y = 0; y < height; ++y) {
        std::fill(row.begin(), row.end(), 0.0F);
        for (int sub = 0; sub < kRows; ++sub) {
            const double sampleY =
                (static_cast<double>(y) + ((static_cast<double>(sub) + 0.5) / kRows)) - halfHeight;
            crossings.clear();
            for (std::size_t i = 0; i + 1 < path.size(); ++i) {
                const auto [ax, ay] = path[i];
                const auto [bx, by] = path[i + 1];
                if ((ay <= sampleY && by <= sampleY) || (ay > sampleY && by > sampleY)) {
                    continue;
                }
                const double t = (sampleY - ay) / (by - ay);
                crossings.push_back(Crossing{ax + (t * (bx - ax)), by > ay ? 1 : -1});
            }
            if (crossings.size() < 2) {
                continue;
            }
            std::sort(crossings.begin(), crossings.end(),
                      [](const Crossing& a, const Crossing& b) { return a.x < b.x; });

            // Nonzero winding: inside wherever the running count is not zero.
            int winding = 0;
            for (std::size_t i = 0; i + 1 < crossings.size(); ++i) {
                winding += crossings[i].direction;
                if (winding == 0) {
                    continue;
                }
                const double from = crossings[i].x + halfWidth;
                const double to = crossings[i + 1].x + halfWidth;
                const auto first = static_cast<std::int32_t>(std::floor(from));
                const auto last = static_cast<std::int32_t>(std::ceil(to));
                for (std::int32_t x = std::max(0, first); x < std::min(width, last); ++x) {
                    // The exact overlap of this span with this pixel, so a
                    // near-vertical edge is smooth without any supersampling
                    // across.
                    const double left = std::max(from, static_cast<double>(x));
                    const double right = std::min(to, static_cast<double>(x) + 1.0);
                    if (right > left) {
                        row[static_cast<std::size_t>(x)] +=
                            static_cast<float>((right - left) / kRows);
                    }
                }
            }
        }
        float* target =
            coverage.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width));
        for (std::int32_t x = 0; x < width; ++x) {
            target[x] = std::clamp(row[static_cast<std::size_t>(x)], 0.0F, 1.0F);
        }
    }
}

void rasteriseMaskPath(const model::MaskPath& path, double feather, bool inverted,
                       std::int32_t width, std::int32_t height, std::vector<float>& coverage) {
    rasterisePath(flatten(path), width, height, coverage);

    if (feather > 0.0 && width > 0 && height > 0) {
        // Feather is a blur of the coverage, using the same separable Gaussian
        // the effect stack uses -- so a feathered path and a feathered
        // rectangle soften at the same rate rather than at two rates nobody
        // chose.
        RgbaImage carrier{width, height};
        for (std::int32_t y = 0; y < height; ++y) {
            Rgba* row = carrier.row(y);
            const float* source =
                coverage.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width));
            for (std::int32_t x = 0; x < width; ++x) {
                row[x] = Rgba{source[x], source[x], source[x], source[x]};
            }
        }
        RgbaImage scratch;
        // A quarter of the feather as the standard deviation: three of those
        // either side is the whole width, which is what the number means on
        // every other feather control here.
        blur(carrier, scratch, static_cast<float>(feather) / 4.0F);
        for (std::int32_t y = 0; y < height; ++y) {
            const Rgba* row = carrier.row(y);
            float* target =
                coverage.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width));
            for (std::int32_t x = 0; x < width; ++x) {
                target[x] = row[x].a;
            }
        }
    }

    if (inverted) {
        for (float& value : coverage) {
            value = 1.0F - value;
        }
    }
}

}  // namespace zaro::render
