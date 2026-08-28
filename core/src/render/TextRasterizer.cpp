#include "zaro/core/render/TextRasterizer.h"

#include <algorithm>
#include <cstdint>

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

model::Graphic captionGraphic(const model::CaptionStyle& style, const std::string& text,
                              std::int32_t frameWidth, std::int32_t frameHeight) {
    model::Graphic graphic;
    graphic.kind = model::GraphicKind::Text;
    graphic.text = text;
    graphic.family = style.family;
    graphic.pointSize = style.pointSize;
    graphic.bold = style.bold;
    graphic.alignment = 0;  // centred, which is where captions go

    graphic.width = frameWidth * std::clamp(style.widthFraction, 0.05, 1.0);
    // Tall enough for several lines, and anchored by its bottom edge: captions
    // grow upwards from the margin, so a two-line caption does not push itself
    // off the bottom of the frame.
    graphic.height = std::max(1.0, style.pointSize * 4.0);
    graphic.centreX = 0.0;
    graphic.centreY = (frameHeight * 0.5) - style.bottomMargin - (graphic.height * 0.5);

    graphic.red = style.red;
    graphic.green = style.green;
    graphic.blue = style.blue;
    graphic.alpha = style.alpha;
    return graphic;
}

}  // namespace zaro::render
