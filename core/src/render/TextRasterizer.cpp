#include "zaro/core/render/TextRasterizer.h"

#include <algorithm>

namespace zaro::render {

bool drawText(const model::Graphic& graphic, TextRasterizer* rasterizer, RgbaImage& out) {
    if (!out.isValid()) {
        return false;
    }
    out.clear();
    if (graphic.kind != model::GraphicKind::Text || graphic.text.empty()) {
        return true;  // nothing to draw is not a failure
    }
    if (rasterizer == nullptr) {
        return false;
    }
    if (Status drawn = rasterizer->renderCoverage(graphic, out); !drawn) {
        return false;
    }

    // Coverage into premultiplied linear colour. The mask's alpha is the only
    // thing read: whatever the engine put in the colour channels was in its own
    // space, and converting it here is exactly the mistake this design avoids.
    const auto alpha = static_cast<float>(std::clamp(graphic.alpha, 0.0, 1.0));
    const auto red = static_cast<float>(graphic.red);
    const auto green = static_cast<float>(graphic.green);
    const auto blue = static_cast<float>(graphic.blue);
    for (std::int32_t y = 0; y < out.height(); ++y) {
        Rgba* row = out.row(y);
        for (std::int32_t x = 0; x < out.width(); ++x) {
            const float coverage = std::clamp(row[x].a, 0.0F, 1.0F) * alpha;
            row[x] = Rgba{red * coverage, green * coverage, blue * coverage, coverage};
        }
    }
    return true;
}

}  // namespace zaro::render
