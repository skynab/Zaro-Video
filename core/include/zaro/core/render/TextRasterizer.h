#pragma once

#include <cstdint>
#include <string>

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
///
/// `reveal` is how much of the line is shown, from none of it to all of it --
/// a typewriter. Applied by cutting the string before the glyphs are laid out
/// rather than by masking the picture afterwards, because those are different
/// pictures: a mask over centred text uncovers the middle of a line that is
/// already fully laid out, and what a typewriter does is put characters down
/// one at a time.
[[nodiscard]] bool drawText(const model::Graphic& graphic, TextRasterizer* rasterizer,
                            RgbaImage& out, double reveal = 1.0);

/// The first `reveal` of a string, by characters rather than by bytes.
///
/// Exposed for the test, and because "how much of this text is that" is the
/// kind of arithmetic that is wrong in a way nobody sees until a title in a
/// language with multi-byte characters comes out cut in half.
[[nodiscard]] std::string revealedText(const std::string& text, double reveal);

/// The graphic a caption is drawn as, at a given frame size.
///
/// Built here rather than in each render graph, so the CPU and the GPU put
/// captions in the same place — which is the whole of what a burned-in caption
/// has to get right.
[[nodiscard]] model::Graphic captionGraphic(const model::CaptionStyle& style,
                                            const std::string& text, std::int32_t frameWidth,
                                            std::int32_t frameHeight);

}  // namespace zaro::render
