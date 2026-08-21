#include "zaro/core/render/Compare.h"

#include <algorithm>
#include <cmath>

#include "zaro/core/render/Compositing.h"

namespace zaro::render {
namespace {

/// The line down the middle.
///
/// Drawn, and deliberately. Without it a split of two similar grades reads as
/// one picture with an odd seam, and somebody spends a while wondering which
/// side they are looking at. One pixel, at full brightness, on the boundary
/// itself.
void drawDivider(RgbaImage& out, std::int32_t x) {
    if (x < 0 || x >= out.width()) {
        return;
    }
    for (std::int32_t y = 0; y < out.height(); ++y) {
        out.row(y)[x] = Rgba{1.0F, 1.0F, 1.0F, 1.0F};
    }
}

}  // namespace

void compareFrames(const RgbaImage& reference, const RgbaImage& current, RgbaImage& out,
                   CompareMode mode, double split) {
    if (!current.isValid()) {
        return;
    }
    if (out.width() != current.width() || out.height() != current.height()) {
        out = RgbaImage{current.width(), current.height()};
    }
    out.clear();

    if (!reference.isValid()) {
        // Nothing to compare against yet. The current frame on its own, rather
        // than half a picture: somebody who has not set a reference should see
        // what they were seeing.
        drawOver(current, out);
        return;
    }

    if (mode == CompareMode::SideBySide) {
        // Each scaled to half width and centred in its half. Half height too,
        // so the aspect ratio survives -- a comparison that stretched one shot
        // would be worse than useless for judging a grade.
        model::Transform left;
        left.scaleX = 0.5;
        left.scaleY = 0.5;
        left.positionX = -static_cast<double>(out.width()) / 4.0;
        model::Transform right = left;
        right.positionX = static_cast<double>(out.width()) / 4.0;

        drawTransformed(reference, out, left);
        drawTransformed(current, out, right);
        drawDivider(out, out.width() / 2);
        return;
    }

    const auto boundary = static_cast<std::int32_t>(
        std::lround(std::clamp(split, 0.0, 1.0) * static_cast<double>(out.width())));
    for (std::int32_t y = 0; y < out.height(); ++y) {
        const Rgba* referenceRow = y < reference.height() ? reference.row(y) : nullptr;
        const Rgba* currentRow = current.row(y);
        Rgba* target = out.row(y);
        for (std::int32_t x = 0; x < out.width(); ++x) {
            if (x < boundary) {
                // Copied rather than sampled: a split exists to show a
                // difference in detail, and resampling one side would invent a
                // difference of its own.
                target[x] =
                    referenceRow != nullptr && x < reference.width() ? referenceRow[x] : Rgba{};
            } else {
                target[x] = currentRow[x];
            }
        }
    }
    drawDivider(out, boundary);
}

}  // namespace zaro::render
