#include "TitlePresets.h"

#include <algorithm>

namespace zaro::app {
namespace {

/// Where a preset sits and how big it is, as fractions of the frame.
///
/// Fractions rather than pixels so that one table serves every sequence size,
/// and so that a lower third is a third of the way up whatever it is cut into.
struct Shape {
    double heightOfText;  ///< text size as a fraction of the frame height
    double boxWidth;      ///< as a fraction of the frame width
    double boxHeight;     ///< as a fraction of the frame height
    double centreY;       ///< positive is down, as a fraction of the frame height
    int alignment;        ///< -1 left, 0 centre, 1 right
};

Shape shapeFor(std::string_view id) {
    if (id == "lower-third") {
        // Down and to the left, where a name and a role go. Not at the very
        // bottom: that is where captions live, and a lower third that collides
        // with the subtitles is the classic delivery note.
        return Shape{1.0 / 18.0, 0.5, 0.14, 0.26, -1};
    }
    if (id == "caption") {
        // Along the bottom, inside the title-safe area, centred.
        return Shape{1.0 / 20.0, 0.8, 0.1, 0.36, 0};
    }
    // A card in the middle of the frame.
    return Shape{1.0 / 12.0, 0.8, 0.2, 0.0, 0};
}

}  // namespace

const std::vector<TitlePreset>& titlePresets() {
    static const std::vector<TitlePreset> presets{
        {"title", "Title", "Centred in the frame"},
        {"lower-third", "Lower third", "Name and role, lower left"},
        {"caption", "Caption", "Along the bottom, centred"},
    };
    return presets;
}

const TitlePreset* findTitlePreset(std::string_view id) {
    const auto& presets = titlePresets();
    const auto found = std::find_if(presets.begin(), presets.end(),
                                    [id](const TitlePreset& preset) { return preset.id == id; });
    return found == presets.end() ? nullptr : &*found;
}

model::Graphic graphicFor(const TitlePreset& preset, std::int32_t frameWidth,
                          std::int32_t frameHeight) {
    const auto width = static_cast<double>(std::max(frameWidth, 1));
    const auto height = static_cast<double>(std::max(frameHeight, 1));
    const Shape shape = shapeFor(preset.id);

    model::Graphic graphic;
    graphic.kind = model::GraphicKind::Text;
    graphic.text = preset.name;
    // No family. The rasteriser then draws in the machine's default, which is
    // certainly installed; choosing one is the first thing the inspector
    // offers, and the choice is stored as a name so it survives the trip to a
    // machine that has a different set of fonts.
    graphic.pointSize = std::max(12.0, height * shape.heightOfText);
    graphic.alignment = shape.alignment;
    graphic.width = width * shape.boxWidth;
    graphic.height = std::max(graphic.pointSize * 2.0, height * shape.boxHeight);
    // Left-aligned presets are inset from the edge rather than centred, so the
    // text starts where the eye expects it and not in the middle of the frame.
    graphic.centreX =
        shape.alignment < 0 ? -(width * 0.5 - graphic.width * 0.5 - width * 0.08) : 0.0;
    graphic.centreY = height * shape.centreY;
    graphic.red = 1.0;
    graphic.green = 1.0;
    graphic.blue = 1.0;
    graphic.alpha = 1.0;
    return graphic;
}

time::RationalTime defaultTitleLength(const time::Rational& rate) {
    return time::RationalTime::fromSeconds(time::Rational{5, 1}, rate);
}

}  // namespace zaro::app
