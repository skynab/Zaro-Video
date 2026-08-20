#pragma once

#include <cstdint>

#include "zaro/core/media/VideoFrame.h"
#include "zaro/core/model/Keyer.h"

namespace zaro::render {

/// A keyer reduced to the numbers a per-pixel test needs.
///
/// The key colour arrives here already converted to linear and already
/// normalised to a chromaticity, so the per-pixel path is a subtraction and a
/// length. The shader receives exactly this, which is what keeps the two
/// implementations from disagreeing about what "tolerance 0.12" means.
struct KeyerConstants {
    /// The key colour with its brightness divided out: r + g + b == 1.
    float keyR{0.0F};
    float keyG{0.0F};
    float keyB{0.0F};

    /// Where the falloff starts and ends, as a distance in that space.
    float tolerance{0.0F};
    float outer{0.0F};

    /// Linear, converted from the display-referred thresholds somebody set.
    float lumaInnerLow{0.0F};
    float lumaOuterLow{0.0F};
    float lumaInnerHigh{0.0F};
    float lumaOuterHigh{0.0F};

    /// Which channel of the key colour dominates: 0 red, 1 green, 2 blue.
    /// Worked out here rather than in the shader so that a key exactly between
    /// two channels cannot be resolved one way on the CPU and the other on the
    /// GPU.
    std::int32_t spillChannel{1};
    float spill{0.0F};

    bool showMatte{false};
    model::KeyKind kind{model::KeyKind::None};

    [[nodiscard]] bool isActive() const noexcept { return kind != model::KeyKind::None; }
};

[[nodiscard]] KeyerConstants keyerConstantsFor(const model::Keyer& keyer,
                                               media::TransferFunction transfer);

/// How much of this pixel survives: 1 keeps it, 0 makes it transparent.
///
/// Takes straight (un-premultiplied) linear values, because a key is a question
/// about the colour of the light and not about how faded the clip is.
[[nodiscard]] float keyMatte(const KeyerConstants& keyer, float r, float g, float b);

/// Pull the key colour out of what survives, in place.
///
/// The dominant channel of the key is clamped towards the mean of the other
/// two, blended by the spill amount. A heuristic, and openly so: separating the
/// light bouncing off the screen from the light that belongs to the subject
/// needs to know what the subject would have looked like, which is exactly the
/// thing nobody has. What this does instead is the operation that removes
/// fringing on real footage, cheaply enough to run per pixel on both paths.
void suppressSpill(const KeyerConstants& keyer, float& r, float& g, float& b);

}  // namespace zaro::render
