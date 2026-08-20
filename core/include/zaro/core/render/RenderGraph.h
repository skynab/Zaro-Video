#pragma once

#include "zaro/core/model/Project.h"
#include "zaro/core/model/Sequence.h"
#include "zaro/core/render/Compositing.h"
#include "zaro/core/render/FrameSource.h"
#include "zaro/core/render/TextRasterizer.h"

namespace zaro::render {

/// Resolves a sequence to a picture at an instant.
///
/// `composite` is pure: the same sequence and the same time always produce the
/// same frame. That is what lets playback be "call this on a clock", export be
/// "call this as fast as possible", and the render cache be memoisation --
/// one code path serving three purposes rather than three that have to be kept
/// agreeing with each other.
class RenderGraph {
public:
    explicit RenderGraph(FrameSource& source) : source_{&source} {}

    /// Render into an existing buffer, reallocating only if the size changed.
    /// The per-frame path in a render loop, where allocating 8 MB a frame would
    /// be the dominant cost.
    [[nodiscard]] Status compositeInto(const model::Sequence& sequence,
                                       const time::RationalTime& at, RgbaImage& out);

    [[nodiscard]] Result<RgbaImage> composite(const model::Sequence& sequence,
                                              const time::RationalTime& at);

    /// How many clips contributed to the last composite. Diagnostics, and the
    /// basis of a "this frame is expensive" indicator later.
    [[nodiscard]] std::int32_t lastClipCount() const noexcept { return lastClipCount_; }

    /// How to draw text, if anything can.

    ///

    /// Null is normal: a headless tool that was never given a font engine renders

    /// the rest of the sequence and counts the text it had to leave out, which is

    /// visible and diagnosable. Drawing an empty frame instead would look like a

    /// bug in the text.

    void setTextRasterizer(TextRasterizer* rasterizer) { text_ = rasterizer; }

    /// The project a nested clip's sequence is looked up in.
    ///
    /// Without it a nested clip has an id and nothing to resolve it against, so
    /// it draws nothing. A graph that was never given a project renders
    /// everything else, which is what the headless tests do.
    void setProject(const model::Project* project) { project_ = project; }

    [[nodiscard]] std::int32_t lastSkippedTextCount() const noexcept { return skippedText_; }

private:
    /// Baked tone curves, kept between frames: building one is thousands of
    /// spline evaluations, and a grade that is not being edited changes on no
    /// frames at all.
    void drawClip(const model::Clip& clip, const RgbaImage& image, RgbaImage& out,
                  const model::Transform& transform, const time::RationalTime& at);
    /// Composite a nested sequence and draw it. False when it cannot be
    /// resolved, which the caller treats as a clip that drew nothing.
    [[nodiscard]] bool compositeNested(const model::Clip& clip, RgbaImage& out,
                                       const time::RationalTime& at);
    /// Grade what has already been composited, in place.
    void applyAdjustment(const model::Clip& clip, RgbaImage& out, const time::RationalTime& at);

    /// Scratch for generated clips, kept between frames so a shape layer does
    /// not allocate a frame-sized buffer on every frame.
    RgbaImage generated_;
    const model::Project* project_{nullptr};
    /// Scratch per nesting level, so a nested composite does not overwrite the
    /// buffer the level above it is still drawing from.
    std::vector<RgbaImage> nestedBuffers_;
    std::int32_t depth_{0};
    TextRasterizer* text_{nullptr};
    std::int32_t skippedText_{0};

    CurveTableCache curves_;
    /// Baked look LUTs, keyed by path, so two clips sharing a look share one
    /// cube and a missing file is not re-opened every frame.
    LutCache luts_;

    /// The curve is drawn against the picture as it is *shown*, so baking it
    /// needs the transfer function the frame is being shown through. Rec.709
    /// until a sequence carries its own; the same default the scopes use.
    media::TransferFunction transfer_{media::TransferFunction::BT709};

    FrameSource* source_;
    std::int32_t lastClipCount_{0};
};

}  // namespace zaro::render
