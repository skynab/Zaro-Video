#include "zaro/core/render/ShapeRaster.h"

#include <algorithm>
#include <cmath>

namespace zaro::render {
namespace {

/// Signed distance to a rounded rectangle, negative inside.
///
/// A distance rather than an inside/outside test, because the distance is what
/// gives both antialiasing and feather for free: coverage is a ramp across it,
/// and the only difference between the two is how wide the ramp is.
double roundedRectDistance(double dx, double dy, double halfWidth, double halfHeight,
                           double radius) {
    const double limit = std::min(halfWidth, halfHeight);
    const double r = std::clamp(radius, 0.0, limit);
    const double qx = std::fabs(dx) - (halfWidth - r);
    const double qy = std::fabs(dy) - (halfHeight - r);
    const double outside = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0));
    const double inside = std::min(std::max(qx, qy), 0.0);
    return outside + inside - r;
}

/// Signed distance to an ellipse, negative inside.
///
/// Approximate: the exact distance to an ellipse has no closed form, and this
/// is the standard gradient-scaled estimate. It is accurate to well under a
/// pixel for the aspect ratios a shape layer is drawn at, and the error is
/// symmetric, so the edge stays where it should be even where its width is
/// slightly off.
double ellipseDistance(double dx, double dy, double halfWidth, double halfHeight) {
    if (halfWidth <= 0.0 || halfHeight <= 0.0) {
        return 1.0;
    }
    const double nx = dx / halfWidth;
    const double ny = dy / halfHeight;
    const double value = std::hypot(nx, ny);
    if (value == 0.0) {
        return -std::min(halfWidth, halfHeight);
    }
    // Scale the normalised distance back by the local gradient.
    const double gradient = std::hypot(nx / halfWidth, ny / halfHeight);
    return (value - 1.0) / gradient;
}

}  // namespace

float shapeCoverage(const model::Graphic& graphic, std::int32_t width, std::int32_t height,
                    double x, double y) {
    if (!graphic.isSet() || width <= 0 || height <= 0) {
        return 0.0F;
    }
    // Pixel centres, and the origin at the centre of the frame -- the same
    // coordinates Transform uses.
    const double dx = (x + 0.5) - (width * 0.5) - graphic.centreX;
    const double dy = (y + 0.5) - (height * 0.5) - graphic.centreY;
    const double halfWidth = graphic.width * 0.5;
    const double halfHeight = graphic.height * 0.5;
    if (halfWidth <= 0.0 || halfHeight <= 0.0) {
        return 0.0F;
    }

    const double distance =
        graphic.kind == model::GraphicKind::Ellipse
            ? ellipseDistance(dx, dy, halfWidth, halfHeight)
            : roundedRectDistance(dx, dy, halfWidth, halfHeight, graphic.cornerRadius);

    // One pixel of ramp for the edge itself, wider if feather asks. Without the
    // one-pixel floor a hard-edged shape is aliased, and a stair-stepped edge
    // is the first thing anyone notices about a generated graphic.
    const double ramp = std::max(1.0, graphic.feather);
    const double coverage = 0.5 - (distance / ramp);
    return static_cast<float>(std::clamp(coverage, 0.0, 1.0));
}

float maskCoverage(const model::Mask& mask, std::int32_t width, std::int32_t height, double x,
                   double y) {
    if (!mask.isSet() || width <= 0 || height <= 0) {
        return 1.0F;  // no mask lets everything through
    }
    const double dx = (x + 0.5) - (width * 0.5) - mask.centreX;
    const double dy = (y + 0.5) - (height * 0.5) - mask.centreY;
    const double halfWidth = mask.width * 0.5;
    const double halfHeight = mask.height * 0.5;
    if (halfWidth <= 0.0 || halfHeight <= 0.0) {
        return mask.inverted ? 1.0F : 0.0F;
    }

    const double distance =
        mask.shape == model::MaskShape::Ellipse
            ? ellipseDistance(dx, dy, halfWidth, halfHeight)
            : roundedRectDistance(dx, dy, halfWidth, halfHeight, mask.cornerRadius);
    const double ramp = std::max(1.0, mask.feather);
    const double coverage = std::clamp(0.5 - (distance / ramp), 0.0, 1.0);
    return static_cast<float>(mask.inverted ? 1.0 - coverage : coverage);
}

void drawShape(const model::Graphic& graphic, RgbaImage& out) {
    if (!out.isValid()) {
        return;
    }
    out.clear();
    if (!graphic.isSet()) {
        return;
    }

    const auto alpha = static_cast<float>(std::clamp(graphic.alpha, 0.0, 1.0));
    const auto red = static_cast<float>(graphic.red);
    const auto green = static_cast<float>(graphic.green);
    const auto blue = static_cast<float>(graphic.blue);

    for (std::int32_t y = 0; y < out.height(); ++y) {
        Rgba* row = out.row(y);
        for (std::int32_t x = 0; x < out.width(); ++x) {
            const float coverage = shapeCoverage(graphic, out.width(), out.height(), x, y);
            if (coverage <= 0.0F) {
                continue;
            }
            // Premultiplied, which is what the compositor expects: coverage and
            // colour scale together, so a feathered edge fades rather than
            // turning grey.
            const float a = coverage * alpha;
            row[x] = Rgba{red * a, green * a, blue * a, a};
        }
    }
}

}  // namespace zaro::render
