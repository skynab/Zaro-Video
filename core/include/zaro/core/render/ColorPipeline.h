#pragma once

#include <cstdint>

#include "zaro/core/Error.h"
#include "zaro/core/media/VideoFrame.h"
#include "zaro/core/render/RgbaImage.h"

namespace zaro::render {

/// Conversion between tagged source frames and the linear working space.
///
/// Symmetric by construction: whatever curve and matrix a frame is tagged with
/// are inverted on the way in and reapplied on the way out, so a clip that
/// passes through untouched comes back bit-identical. See docs/adr/0005.

/// Decode a source frame into scene-linear premultiplied RGBA.
///
/// Fully opaque: source video has no alpha channel, so alpha is 1 everywhere
/// and premultiplication is a no-op at this stage. It stops being a no-op the
/// moment anything is scaled, rotated or faded.
[[nodiscard]] Status toLinear(const media::VideoFrame& source, RgbaImage& out);

/// Encode the working space back to 8-bit display-referred RGB.
///
/// Values outside [0,1] are clipped, which is the correct behaviour for an
/// 8-bit deliverable and the wrong one for a grading pipeline; tone mapping
/// belongs in a node, not in the encoder.
[[nodiscard]] Status toDisplayRgb24(
    const RgbaImage& source, std::uint8_t* destination, std::int32_t strideBytes,
    media::TransferFunction transfer = media::TransferFunction::BT709);

/// The scalar conversions, exposed for testing and for the shader ports to
/// check themselves against.
[[nodiscard]] float toLinearScalar(float encoded, media::TransferFunction transfer);
[[nodiscard]] float fromLinearScalar(float linear, media::TransferFunction transfer);

/// Whether this build can handle a frame's tags at all. HDR transfer curves are
/// recognised and rejected rather than silently treated as Rec.709, which would
/// look catastrophic instead of subtly wrong.
[[nodiscard]] bool isSupported(const media::ColorInfo& color);

}  // namespace zaro::render
