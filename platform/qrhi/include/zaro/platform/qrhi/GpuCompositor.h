#pragma once

#include <memory>
#include <string>

#include "zaro/core/Error.h"
#include "zaro/core/media/VideoFrame.h"
#include "zaro/core/model/ClipEffects.h"
#include "zaro/core/render/RgbaImage.h"

namespace zaro::platform::qrhi {

/// The compositor from ADR-002, on the GPU.
///
/// Same operation as `render::drawTransformed`, same working space: linear
/// float RGBA, premultiplied. The CPU version stays as the reference the
/// golden-frame tests compare against, because a GPU renderer with no
/// independent oracle is one nobody can prove anything about.
///
/// Frames are uploaded from `RgbaImage` for now. That is a round trip this
/// design exists to remove -- the point of the GPU path is that a
/// hardware-decoded frame never leaves the GPU at all -- but doing it in this
/// order isolates the compositing arithmetic from the decode plumbing, so a
/// discrepancy can only be one thing.
class GpuCompositor {
public:
    /// Fails rather than falling back to software: a caller that asked for the
    /// GPU should find out that it did not get it.
    static Result<std::unique_ptr<GpuCompositor>> create();
    ~GpuCompositor();

    /// Name of the backend actually in use, for diagnostics.
    [[nodiscard]] std::string backendName() const;

    /// Start a frame, clearing to transparent black.
    [[nodiscard]] Status beginFrame(std::int32_t width, std::int32_t height);

    /// Composite one source image under a transform.
    [[nodiscard]] Status draw(const render::RgbaImage& source, const model::Transform& transform,
                              model::BlendMode blend = model::BlendMode::Normal);

    /// Composite a decoded frame directly, converting Y'CbCr to the working
    /// space in the same shader pass.
    ///
    /// This is the path that makes the GPU worth using. The planes go up as the
    /// decoder produced them -- about 3 MB rather than 8 MB at 1080p -- and the
    /// colour conversion costs nothing extra, because the fragment shader is
    /// already sampling. See docs/adr/0007.
    [[nodiscard]] Status drawSource(const media::VideoFrame& source,
                                    const model::Transform& transform,
                                    model::BlendMode blend = model::BlendMode::Normal);

    /// Finish, and bring the result back to the CPU.
    ///
    /// The readback is here so the result can be compared and encoded. In a
    /// preview path it is exactly what should not happen; the texture should go
    /// straight to the screen.
    [[nodiscard]] Status endFrame(render::RgbaImage& out);

    /// Finish without reading back — what a preview does, where the result
    /// stays on the GPU. Exposed so the cost of the readback can be measured
    /// separately from the cost of the work.
    [[nodiscard]] Status endFrameOnGpu();

private:
    GpuCompositor();

    /// Replay the frame's recorded draws inside a single render pass.
    void submitPass();

    /// Build the compositing pipeline for a blend mode, once.
    [[nodiscard]] Status ensureCompositePipeline(std::size_t blendIndex);

    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace zaro::platform::qrhi
