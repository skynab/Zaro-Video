#pragma once

#include <cstdint>
#include <memory>

#include "zaro/core/model/Sequence.h"
#include "zaro/core/render/CurveTable.h"
#include "zaro/core/render/FrameSource.h"
#include "zaro/core/render/RenderCache.h"
#include "zaro/core/render/RenderGraph.h"
#include "zaro/core/render/TextRasterizer.h"
#include "zaro/core/render/TransitionShape.h"
#include "zaro/platform/qrhi/GpuCompositor.h"

namespace zaro::platform::qrhi {

/// `render::RenderGraph`, resolved on the GPU.
///
/// Same traversal, same rules: video tracks bottom-up, muted tracks and
/// disabled clips skipped, an unreadable clip leaves a hole rather than failing
/// the frame. What differs is that the decoder's planes go straight to the GPU
/// and the result never has to come back.
///
/// Measured at 1080p, converting and compositing: the CPU pipeline manages
/// 39.9 fps, this path 479.5 fps when the result stays on the GPU, and 94.0 fps
/// when it is read back. The gap between the last two is the readback, and it
/// is why `composite` and `compositeInto` are separate calls rather than one
/// with a flag.
class GpuRenderGraph {
public:
    /// Nested sequences are composited on the CPU and uploaded.
    ///
    /// A nest would otherwise need the compositor to render into a texture and
    /// then read from it, which means a second target to ping-pong against --
    /// real plumbing for a case that is rare in a preview and never on the path
    /// that delivers. The export renderer is the CPU one anyway, so this is the
    /// slow path being slow rather than a second implementation.
    void setNestedSource(render::FrameSource* source) { nestedSource_ = source; }

    /// The project nested clips are resolved against.
    void setProject(const model::Project* project) { project_ = project; }

    /// How to draw text. Set by whoever owns a font engine.
    void setTextRasterizer(render::TextRasterizer* rasterizer) { text_ = rasterizer; }

    /// Where the CPU fallback memoises its frames.
    ///
    /// The GPU path never reads this, and does not need to: it is already fast.
    /// What is slow is the whole-frame CPU composite an adjustment layer
    /// forces, and that is what a cached frame replaces.
    void setRenderCache(render::RenderCache* cache) {
        cache_ = cache;
        if (nested_ != nullptr) {
            nested_->setRenderCache(cache);
        }
    }

    GpuRenderGraph(GpuCompositor& compositor, render::SourceFrameProvider& provider)
        : compositor_{&compositor}, provider_{&provider} {}

    /// Composite a frame and leave it on the GPU. What a preview wants.
    [[nodiscard]] Status composite(const model::Sequence& sequence, const time::RationalTime& at);

    /// Composite inside a frame someone else opened, leaving the result on the
    /// GPU. What a preview widget wants: it is already mid-frame when it asks.
    [[nodiscard]] Status compositeOn(::QRhiCommandBuffer* commandBuffer,
                                     const model::Sequence& sequence, const time::RationalTime& at);

    /// Composite a frame and bring it back. What an export wants.
    [[nodiscard]] Status compositeInto(const model::Sequence& sequence,
                                       const time::RationalTime& at, render::RgbaImage& out);

    [[nodiscard]] std::int32_t lastClipCount() const noexcept { return lastClipCount_; }

private:
    [[nodiscard]] Status drawClips(const model::Sequence& sequence, const time::RationalTime& at);

    /// Baked tone curves, kept between frames for the same reason the CPU

    /// graph keeps them: building one is thousands of spline evaluations.

    /// Draw a clip whose picture is already an image -- a generated shape, or a
    /// nested sequence composited on the CPU.
    /// Whether anything live at this moment is beyond what one queued sampling
    /// pass can do -- an adjustment layer, or an effect -- which sends the
    /// whole frame down the CPU path.
    [[nodiscard]] static bool needsCpuFallback(const model::Sequence& sequence,
                                               const time::RationalTime& at);

    bool drawClipImage(const model::Clip& clip, const render::RgbaImage& image,
                       const model::Transform& transform, const time::RationalTime& at,
                       const model::Mask* wipe = nullptr);

    bool drawClip(const model::Clip& clip, const media::VideoFrame& frame,
                  const model::Transform& transform, const time::RationalTime& at,
                  const model::Mask* wipe = nullptr);

    render::RgbaImage generated_;
    /// A second buffer, so both halves of a transition can be generated.
    render::RgbaImage generatedB_;

    /// Draw one side of a transition, whether its picture is read or made.
    ///
    /// The transition branch used to reach straight for the decoder, so a
    /// dissolve between two title cards drew nothing -- silently, because a
    /// clip whose media cannot be read is treated as a gap.
    bool drawTransitionSide(const model::Clip& clip, const model::Sequence& sequence,
                            const model::Transform& transform, const time::RationalTime& at,
                            render::RgbaImage& scratch, const model::Mask* wipe);
    const model::Project* project_{nullptr};
    render::FrameSource* nestedSource_{nullptr};
    std::unique_ptr<render::RenderGraph> nested_;

public:
    /// The CPU compositor this graph falls back to, building it if it has not
    /// been needed yet. Null when there is nothing for it to read.
    ///
    /// Exposed because comparison view needs two composites of one sequence at
    /// two instants, which is the same reason the fallback exists at all.
    [[nodiscard]] render::RenderGraph* cpuGraph();

private:
    render::RenderCache* cache_{nullptr};
    render::TextRasterizer* text_{nullptr};
    render::CurveTableCache curves_;
    /// The same, for the hue curve. Separate for the reason the CPU graph
    /// keeps them apart: the two are rebuilt by different edits, and one cache
    /// would rebuild both every time either moved.
    render::ColorCurveTableCache colorCurves_;
    render::LutCache luts_;

    media::TransferFunction transfer_{media::TransferFunction::BT709};

    GpuCompositor* compositor_;
    render::SourceFrameProvider* provider_;
    std::int32_t lastClipCount_{0};
};

}  // namespace zaro::platform::qrhi
