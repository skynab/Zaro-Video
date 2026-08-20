#pragma once

#include "zaro/core/render/TextRasterizer.h"

namespace zaro::platform::qtext {

/// Glyph rasterisation on Qt's font engine.
///
/// Qt Gui only -- no widgets. That keeps it usable from the export tool, which
/// has no window and must not need one to put text in a delivered file.
class QtTextRasterizer final : public render::TextRasterizer {
public:
    [[nodiscard]] Status renderCoverage(const model::Graphic& graphic,
                                        render::RgbaImage& coverage) override;
};

}  // namespace zaro::platform::qtext
