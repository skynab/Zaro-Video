#pragma once

#include <string>

#include "zaro/core/render/RgbaImage.h"

namespace zaro::render {

/// The bit of picture a track follows, in image pixels from the top left.
///
/// A rectangle rather than the mask's own outline: what is being followed is a
/// piece of texture, and the cheapest honest description of a piece of texture
/// is the box around it. A track that sampled only the pixels inside a bezier
/// would spend its time on the shape and not on the motion, and would still
/// have to fall back to a box the moment the shape had no detail in it.
struct PatchWindow {
    double centreX{0.0};
    double centreY{0.0};
    double halfWidth{40.0};
    double halfHeight{40.0};
    /// How far the patch is allowed to have moved between the two frames. Too
    /// small loses fast motion; too large is slow and, worse, finds matches
    /// somewhere else in a repeating pattern.
    double search{24.0};
};

/// Where the patch went, and whether to believe it.
struct PatchTrack {
    double dx{0.0};
    double dy{0.0};
    /// The correlation at the best offset, in [-1, 1]. Reported rather than
    /// reduced to a verdict: a track that is drifting is still worth showing
    /// somebody, and the number is what tells them it is drifting.
    double confidence{0.0};
    bool usable{false};
    std::string reason;
};

/// Follow the patch from `from` into `to`.
///
/// **Zero-mean normalised cross-correlation, not sum of differences.** A shot
/// that brightens between two frames -- an auto-exposure step, a lamp coming
/// on, a dissolve starting -- shifts every pixel of the patch by the same
/// amount, and a difference metric reads that as the patch having gone
/// somewhere. Correlation subtracts the mean and divides by the spread, so a
/// gain or lift change scores the same as no change at all.
///
/// **Translation only.** Rotation and scale are not estimated: one patch
/// cannot separate them from translation, and the usual answer -- several
/// patches solving a similarity together -- is a different feature with its own
/// failure modes. What this is for is a mask that has to sit on something that
/// moves, and translation is what that mostly is.
///
/// **The patch is subsampled to a bounded grid.** A mask covering half a frame
/// would otherwise make one track cost a billion multiplies. At most 64 by 64
/// samples are taken across the window however big it is, which is far more
/// than the motion needs and keeps a large mask the same price as a small one.
///
/// Refuses when the patch is flat -- nothing to lock onto -- or when the best
/// correlation is too poor to mean anything, saying which in `reason`.
[[nodiscard]] PatchTrack trackPatch(const RgbaImage& from, const RgbaImage& to,
                                    const PatchWindow& window);

}  // namespace zaro::render
