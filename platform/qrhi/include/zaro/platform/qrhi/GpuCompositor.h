#pragma once

#include <memory>
#include <string>

#include "zaro/core/Error.h"
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

    /// Finish, and bring the result back to the CPU.
    ///
    /// The readback is here so the result can be compared and encoded. In a
    /// preview path it is exactly what should not happen; the texture should go
    /// straight to the screen.
    [[nodiscard]] Status endFrame(render::RgbaImage& out);

private:
    GpuCompositor();

    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace zaro::platform::qrhi
