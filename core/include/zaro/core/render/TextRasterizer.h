#pragma once

#include "zaro/core/Error.h"
#include "zaro/core/model/Caption.h"
#include "zaro/core/model/Graphic.h"
#include "zaro/core/render/RgbaImage.h"

namespace zaro::render {

/// Draws text into a frame.
///
/// An interface, because `core/` has no font engine and is not going to get
/// one: it links neither Qt nor FFmpeg, and text is exactly the kind of thing
/// that would drag a toolkit in through the back door. The implementation lives
/// in `platform/`, and anything that renders a sequence containing text has to
/// be given one.
///
/// **The font engine produces coverage, not colour.** An implementation renders
/// the glyphs as an alpha mask and this layer multiplies by the graphic's
/// colour in linear light. Asking the engine for coloured text and converting
/// the result from sRGB per pixel would convert the antialiased edges as if
/// they were colours, which is the reason text composited in a linear pipeline
/// so often comes out looking thin.
class TextRasterizer {
public:
    TextRasterizer() = default;
    TextRasterizer(const TextRasterizer&) = delete;
    TextRasterizer& operator=(const TextRasterizer&) = delete;
    TextRasterizer(TextRasterizer&&) = delete;
    TextRasterizer& operator=(TextRasterizer&&) = delete;
    virtual ~TextRasterizer() = default;

    /// Fill `coverage` -- a single-channel mask in the alpha of an RgbaImage,
    /// sized to the frame -- with the graphic's text laid out in its box.
    ///
    /// Only alpha is read by the caller. An implementation may leave the colour
    /// channels as it likes.
    [[nodiscard]] virtual Status renderCoverage(const model::Graphic& graphic,
                                                RgbaImage& coverage) = 0;
};

/// Draw a text graphic, given something that can rasterise glyphs.
///
/// Returns false when there is no rasteriser, which is not an error: a headless
/// tool that was never given one renders the rest of the sequence and leaves
/// the text out, which is visible and diagnosable. Silently drawing an empty
/// frame instead would look like a bug in the text.
[[nodiscard]] bool drawText(const model::Graphic& graphic, TextRasterizer* rasterizer,
                            RgbaImage& out);

/// The graphic a caption is drawn as, at a given frame size.
///
/// Built here rather than in each render graph, so the CPU and the GPU put
/// captions in the same place — which is the whole of what a burned-in caption
/// has to get right.
[[nodiscard]] model::Graphic captionGraphic(const model::CaptionStyle& style,
                                            const std::string& text, std::int32_t frameWidth,
                                            std::int32_t frameHeight);

}  // namespace zaro::render
