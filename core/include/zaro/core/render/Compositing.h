#pragma once

#include <cstdint>

#include "zaro/core/model/ClipEffects.h"
#include "zaro/core/model/Mask.h"
#include "zaro/core/model/Vignette.h"
#include "zaro/core/render/Grade.h"
#include "zaro/core/render/Keyer.h"
#include "zaro/core/render/RgbaImage.h"

namespace zaro::render {

/// Draw `source` into `destination` under `transform`, compositing with
/// `blend`. Both are premultiplied linear RGBA.
///
/// Implemented as an inverse map: for each destination pixel, work out where it
/// came from in the source and sample there. That is what a GPU does, so the
/// shader port produces the same picture, and it avoids the gaps a forward map
/// leaves when scaling up.
/// A grade, if there is one, is applied to each *sampled* colour rather than to
/// the source beforehand. That is what the shader does — it grades what comes
/// back from the texture unit — and the two have to agree. It also means a
/// grade costs nothing for the pixels a transform never samples.
/// Everything one clip does to one pixel, in one argument.
///
/// Collected rather than passed positionally, and not for tidiness. The list
/// had grown to six nullable pointers in a row, which is the exact shape that
/// once let a patch add colour correction to two of three draw sites and miss
/// the third -- a transition that went two phases with its outgoing half
/// ungraded. A struct makes the call sites read as what they set rather than as
/// a column of nullptrs, and makes adding a stage one field instead of one more
/// place to get the order wrong.
///
/// A default-constructed value means no shading at all, so a caller that only
/// wants geometry passes nothing.
struct ClipShading {
    const GradeConstants* grade{nullptr};
    const CurveTable* curves{nullptr};
    const SecondaryConstants* secondary{nullptr};
    const LutTable* lut{nullptr};
    float lutAmount{1.0F};
    const model::Mask* mask{nullptr};
    const KeyerConstants* keyer{nullptr};
    const model::Vignette* vignette{nullptr};
    /// A second mask, from a wipe. Its coverage multiplies the clip's own,
    /// because a masked clip in a wipe shows where the mask says *and* where
    /// the wipe has got to -- replacing one with the other would silently drop
    /// whichever came second.
    const model::Mask* wipe{nullptr};

    /// A path mask's coverage, already rasterised, one float per output pixel.
    ///
    /// A buffer rather than a shape, because a path's coverage cannot be
    /// answered from a formula per pixel the way a rectangle's can: it is a
    /// scanline fill, done once for the frame. `pathWidth` is its stride.
    const float* pathCoverage{nullptr};
    std::int32_t pathWidth{0};

    [[nodiscard]] bool keying() const noexcept { return keyer != nullptr && keyer->isActive(); }
    [[nodiscard]] bool masking() const noexcept { return mask != nullptr && mask->isSet(); }
    [[nodiscard]] bool vignetting() const noexcept {
        return vignette != nullptr && vignette->isSet();
    }
    [[nodiscard]] bool wiping() const noexcept { return wipe != nullptr && wipe->isSet(); }
    [[nodiscard]] bool pathMasking() const noexcept {
        return pathCoverage != nullptr && pathWidth > 0;
    }
    [[nodiscard]] float pathAt(std::int32_t x, std::int32_t y) const {
        return pathCoverage[(static_cast<std::size_t>(y) * static_cast<std::size_t>(pathWidth)) +
                            static_cast<std::size_t>(x)];
    }
};

void drawTransformed(const RgbaImage& source, RgbaImage& destination,
                     const model::Transform& transform,
                     model::BlendMode blend = model::BlendMode::Normal,
                     const ClipShading& shading = {});

/// Composite `source` over `destination` with no geometry -- the common case,
/// and much faster than going through the sampler.
void drawOver(const RgbaImage& source, RgbaImage& destination, double opacity = 1.0,
              model::BlendMode blend = model::BlendMode::Normal, const ClipShading& shading = {});

}  // namespace zaro::render
