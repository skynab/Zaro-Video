#pragma once

#include <memory>

#include "zaro/core/model/Sequence.h"
#include "zaro/core/render/CurveTable.h"
#include "zaro/core/render/FrameSource.h"
#include "zaro/core/render/RenderGraph.h"
#include "zaro/core/render/TextRasterizer.h"
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
    bool drawClipImage(const model::Clip& clip, const render::RgbaImage& image,
                       const model::Transform& transform, const time::RationalTime& at);

    bool drawClip(const model::Clip& clip, const media::VideoFrame& frame,
                  const model::Transform& transform, const time::RationalTime& at);

    render::RgbaImage generated_;
    const model::Project* project_{nullptr};
    render::FrameSource* nestedSource_{nullptr};
    std::unique_ptr<render::RenderGraph> nested_;
    render::TextRasterizer* text_{nullptr};
    render::CurveTableCache curves_;
    render::LutCache luts_;

    media::TransferFunction transfer_{media::TransferFunction::BT709};

    GpuCompositor* compositor_;
    render::SourceFrameProvider* provider_;
    std::int32_t lastClipCount_{0};
};

}  // namespace zaro::platform::qrhi
