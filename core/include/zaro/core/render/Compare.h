#pragma once

#include "zaro/core/render/RgbaImage.h"

namespace zaro::render {

/// How two frames are shown against each other.
enum class CompareMode : std::uint8_t {
    /// One either side of a moving divide, both at full size. The only
    /// arrangement where a difference in a *detail* is visible, because the
    /// same pixels sit next to each other.
    Split,
    /// Both scaled to fit half the frame. Loses resolution and gains context:
    /// what a split cannot show is a difference in framing or in something the
    /// divide happens to be sitting on.
    SideBySide,
};

/// Put a reference frame and the current one in one picture.
///
/// **A viewing arrangement, not a render.** Nothing here reaches an export: the
/// comparison exists so somebody can judge a grade against something, and what
/// they deliver is the graded shot on its own. That is why it lives beside the
/// compositor rather than in it, and why the reference is not part of the cut.
///
/// `split` is where the divide sits, 0 to 1 across the frame. In `SideBySide`
/// it is ignored -- the halves are the halves.
void compareFrames(const RgbaImage& reference, const RgbaImage& current, RgbaImage& out,
                   CompareMode mode, double split);

}  // namespace zaro::render
