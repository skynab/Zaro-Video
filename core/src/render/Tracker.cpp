#include "zaro/core/render/Tracker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace zaro::render {
namespace {

/// At most this many samples across the patch in each direction. See the
/// header: it is what keeps a mask over half the frame from costing a
/// thousand times what a small one does.
constexpr int kMaxSamples = 64;

/// Below this the patch is flat enough that any offset correlates with any
/// other, so the answer would be noise wearing a number.
constexpr double kMinSpread = 1e-4;

/// Below this the best offset found is not a match, it is the least bad of a
/// bad set. Chosen low enough to survive real footage -- grain, compression,
/// a little motion blur -- and high enough to reject a patch that left frame.
constexpr double kMinConfidence = 0.5;

[[nodiscard]] double luma(const Rgba& pixel) {
    // Unpremultiplied would be the colour of the object; premultiplied is the
    // colour of the picture, which is what the next frame will also show.
    return (0.2126 * static_cast<double>(pixel.r)) + (0.7152 * static_cast<double>(pixel.g)) +
           (0.0722 * static_cast<double>(pixel.b));
}

/// The sample grid over the window: positions, not values, so the same grid
/// can be read out of both frames at different offsets.
struct Grid {
    std::vector<std::int32_t> xs;
    std::vector<std::int32_t> ys;
    std::int32_t stepX{1};
    std::int32_t stepY{1};
};

[[nodiscard]] std::vector<std::int32_t> axis(double centre, double half, std::int32_t limit,
                                             std::int32_t& step) {
    const auto first = static_cast<std::int32_t>(std::floor(centre - half));
    const auto last = static_cast<std::int32_t>(std::ceil(centre + half));
    const std::int32_t span = std::max(1, last - first);
    step = std::max(1, (span + kMaxSamples - 1) / kMaxSamples);
    std::vector<std::int32_t> out;
    for (std::int32_t at = first; at <= last; at += step) {
        if (at >= 0 && at < limit) {
            out.push_back(at);
        }
    }
    return out;
}

/// One channel of a frame, so the correlation loop reads floats out of a flat
/// buffer rather than four-channel pixels it has to weigh every time.
struct Plane {
    std::vector<float> values;
    std::int32_t width{0};
    std::int32_t height{0};

    [[nodiscard]] float at(std::int32_t x, std::int32_t y) const {
        return values[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
                      static_cast<std::size_t>(x)];
    }
};

/// Average over a box, in place, one axis at a time.
///
/// **Because the patch is sampled sparsely.** Reading every nineteenth pixel of
/// a sharp picture is aliasing: the samples land on different parts of an edge
/// at each candidate offset, so the correlation surface picks up a texture of
/// its own that has nothing to do with where anything went. Blurring by the
/// same amount the sampling skips is what makes the sparse samples stand for
/// the pixels between them -- and it costs one pass over the frame, against a
/// full-resolution correlation's cost of one pass per candidate offset.
void blur(Plane& plane, std::int32_t radiusX, std::int32_t radiusY) {
    std::vector<float> scratch(plane.values.size(), 0.0F);
    if (radiusX > 0) {
        for (std::int32_t y = 0; y < plane.height; ++y) {
            for (std::int32_t x = 0; x < plane.width; ++x) {
                const std::int32_t from = std::max(0, x - radiusX);
                const std::int32_t to = std::min(plane.width - 1, x + radiusX);
                float sum = 0.0F;
                for (std::int32_t at = from; at <= to; ++at) {
                    sum += plane.at(at, y);
                }
                scratch[(static_cast<std::size_t>(y) * static_cast<std::size_t>(plane.width)) +
                        static_cast<std::size_t>(x)] = sum / static_cast<float>(to - from + 1);
            }
        }
        plane.values.swap(scratch);
    }
    if (radiusY > 0) {
        for (std::int32_t y = 0; y < plane.height; ++y) {
            const std::int32_t from = std::max(0, y - radiusY);
            const std::int32_t to = std::min(plane.height - 1, y + radiusY);
            for (std::int32_t x = 0; x < plane.width; ++x) {
                float sum = 0.0F;
                for (std::int32_t at = from; at <= to; ++at) {
                    sum += plane.at(x, at);
                }
                scratch[(static_cast<std::size_t>(y) * static_cast<std::size_t>(plane.width)) +
                        static_cast<std::size_t>(x)] = sum / static_cast<float>(to - from + 1);
            }
        }
        plane.values.swap(scratch);
    }
}

[[nodiscard]] Plane planeOf(const RgbaImage& image) {
    Plane plane;
    plane.width = image.width();
    plane.height = image.height();
    plane.values.resize(static_cast<std::size_t>(plane.width) *
                        static_cast<std::size_t>(plane.height));
    std::size_t index = 0;
    for (std::int32_t y = 0; y < plane.height; ++y) {
        const Rgba* row = image.row(y);
        for (std::int32_t x = 0; x < plane.width; ++x) {
            plane.values[index++] = static_cast<float>(luma(row[x]));
        }
    }
    return plane;
}

/// Correlation of the patch against `to` shifted by (ox, oy), or nothing when
/// the shifted patch does not fit in the frame.
///
/// Refusing rather than clamping at the edge: a clamped read repeats the border
/// pixel, which correlates beautifully with itself and would make the edge of
/// the frame the most attractive place for a track to go.
[[nodiscard]] double correlate(const Plane& to, const Grid& grid, const std::vector<double>& patch,
                               double patchMean, double patchSpread, std::int32_t ox,
                               std::int32_t oy) {
    const std::size_t count = patch.size();
    double sum = 0.0;
    double sumSquares = 0.0;
    std::vector<double> other(count, 0.0);
    std::size_t index = 0;
    for (const std::int32_t y : grid.ys) {
        const std::int32_t sy = y + oy;
        if (sy < 0 || sy >= to.height) {
            return -1.0;
        }
        for (const std::int32_t x : grid.xs) {
            const std::int32_t sx = x + ox;
            if (sx < 0 || sx >= to.width) {
                return -1.0;
            }
            const double value = static_cast<double>(to.at(sx, sy));
            other[index++] = value;
            sum += value;
            sumSquares += value * value;
        }
    }
    const auto n = static_cast<double>(count);
    const double mean = sum / n;
    const double spread = std::sqrt(std::max(0.0, (sumSquares / n) - (mean * mean)));
    if (spread < kMinSpread) {
        return 0.0;
    }
    double covariance = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        covariance += (patch[i] - patchMean) * (other[i] - mean);
    }
    return covariance / (n * patchSpread * spread);
}

/// Where the peak of a parabola through three samples sits, as an offset from
/// the middle one, clamped to the samples it was fitted from.
///
/// The correlation surface near a good match is smooth, so the integer peak is
/// almost never where the real one is -- a mask that could only sit on whole
/// pixels would visibly chatter against the thing it is tracking.
[[nodiscard]] double subpixel(double before, double peak, double after) {
    const double denominator = before - (2.0 * peak) + after;
    if (std::fabs(denominator) < 1e-12) {
        return 0.0;
    }
    return std::clamp(0.5 * (before - after) / denominator, -1.0, 1.0);
}

}  // namespace

PatchTrack trackPatch(const RgbaImage& from, const RgbaImage& to, const PatchWindow& window) {
    PatchTrack result;
    if (!from.isValid() || !to.isValid() || from.width() != to.width() ||
        from.height() != to.height()) {
        result.reason = "the two frames are not the same size";
        return result;
    }

    Grid grid;
    grid.xs = axis(window.centreX, window.halfWidth, from.width(), grid.stepX);
    grid.ys = axis(window.centreY, window.halfHeight, from.height(), grid.stepY);
    if (grid.xs.size() < 3 || grid.ys.size() < 3) {
        result.reason = "the mask is too small, or off the edge of the frame, to track";
        return result;
    }

    Plane before = planeOf(from);
    Plane after = planeOf(to);
    blur(before, grid.stepX / 2, grid.stepY / 2);
    blur(after, grid.stepX / 2, grid.stepY / 2);

    std::vector<double> patch;
    patch.reserve(grid.xs.size() * grid.ys.size());
    double sum = 0.0;
    double sumSquares = 0.0;
    for (const std::int32_t y : grid.ys) {
        for (const std::int32_t x : grid.xs) {
            const double value = static_cast<double>(before.at(x, y));
            patch.push_back(value);
            sum += value;
            sumSquares += value * value;
        }
    }
    const auto n = static_cast<double>(patch.size());
    const double mean = sum / n;
    const double spread = std::sqrt(std::max(0.0, (sumSquares / n) - (mean * mean)));
    if (spread < kMinSpread) {
        result.reason = "there is nothing inside the mask to track -- it is flat";
        return result;
    }

    const auto reach = static_cast<std::int32_t>(std::max(1.0, std::round(window.search)));
    double best = -2.0;
    std::int32_t bestX = 0;
    std::int32_t bestY = 0;
    // Every offset in the window, rather than a pyramid. A coarse pass on a
    // downsampled frame is the usual speed-up and it is also how a tracker
    // learns to prefer a wrong answer: the coarse level cannot see the detail
    // that distinguishes two similar places, and the fine level only ever
    // refines what the coarse one chose. The patch is already subsampled, so
    // exhaustive is affordable here.
    for (std::int32_t oy = -reach; oy <= reach; ++oy) {
        for (std::int32_t ox = -reach; ox <= reach; ++ox) {
            const double score = correlate(after, grid, patch, mean, spread, ox, oy);
            if (score > best) {
                best = score;
                bestX = ox;
                bestY = oy;
            }
        }
    }

    result.confidence = best;
    result.dx = static_cast<double>(bestX);
    result.dy = static_cast<double>(bestY);
    if (best < kMinConfidence) {
        result.reason = "lost it -- nothing in the next frame looks like what the mask was on";
        return result;
    }

    // Refined against the neighbours of the peak, one axis at a time. Both
    // neighbours have to exist: at the edge of the search window there is no
    // parabola to fit, and the honest answer there is the integer offset.
    if (bestX > -reach && bestX < reach) {
        result.dx += subpixel(correlate(after, grid, patch, mean, spread, bestX - 1, bestY), best,
                              correlate(after, grid, patch, mean, spread, bestX + 1, bestY));
    }
    if (bestY > -reach && bestY < reach) {
        result.dy += subpixel(correlate(after, grid, patch, mean, spread, bestX, bestY - 1), best,
                              correlate(after, grid, patch, mean, spread, bestX, bestY + 1));
    }
    result.usable = true;
    return result;
}

}  // namespace zaro::render
