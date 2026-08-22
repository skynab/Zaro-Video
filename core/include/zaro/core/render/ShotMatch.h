#pragma once

#include <string>

#include "zaro/core/Error.h"
#include "zaro/core/model/ColorCorrection.h"
#include "zaro/core/render/RgbaImage.h"

namespace zaro::render {

/// What matching one shot to another came up with.
struct ShotMatch {
    /// The correction, as an ASC CDL -- the same three wheels somebody would
    /// have set by hand, so the answer can be nudged afterwards rather than
    /// accepted or thrown away.
    model::ColorWheels wheels;

    /// How far apart the two shots were at the anchors, before and after, in
    /// the same units as the picture. Reported rather than reduced to a verdict
    /// because "how much did this move" is the question somebody actually has.
    double before{0.0};
    double after{0.0};

    /// False when the two frames are too unalike for the answer to mean
    /// anything, with `reason` saying why. The correction is still filled in:
    /// somebody who wants it anyway can have it, but nothing applies it on
    /// their behalf.
    bool usable{false};
    std::string reason;
};

/// Work out a correction that makes `target` sit where `reference` does.
///
/// **It matches three anchors per channel, not the average.** Matching means
/// alone moves a shot bodily and leaves its contrast wrong; matching means and
/// spreads gets the contrast but pins nothing in particular. Three points --
/// a shadow, a midtone and a highlight -- are what a colourist is actually
/// looking at, and they map exactly onto the three terms of a CDL: the slope
/// and offset put the two ends where they belong, and the power takes the
/// middle.
///
/// **It does not look at the pictures, only at their distributions.** Two shots
/// of the same scene from different angles have different content and the same
/// palette, which is the case this is for. The consequence is the honest
/// limitation: matching a close-up of a face to a wide of a landscape produces
/// a confident answer to a question nobody asked, so when the two
/// distributions are too unalike the result is marked unusable rather than
/// applied.
[[nodiscard]] Result<ShotMatch> matchShot(const RgbaImage& reference, const RgbaImage& target);

}  // namespace zaro::render
