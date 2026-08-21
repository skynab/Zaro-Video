#pragma once

#include "zaro/core/render/RgbaImage.h"

namespace zaro::render {

/// Fit scene light into what a display can show.
///
/// **Not a look.** A look is a choice about how something should feel; this is
/// the last step that makes a picture fit the range it is going out in, and it
/// exists because the working space carries light above white -- a specular
/// highlight, a practical lamp, anything a log curve preserved -- and an 8-bit
/// deliverable does not.
///
/// **Exactly the identity below the knee.** Everything at or under it comes out
/// bit for bit unchanged, so a project that never exceeds white exports the
/// same file it always did. That is not an optimisation: a tone map that
/// touched the midtones would silently change every existing deliverable, and
/// the first anybody would know is a re-export not matching the one signed off.
///
/// Above the knee the curve is compressive and approaches 1 without reaching
/// it, so nothing clips and no two different highlights come out the same
/// value. It meets the identity with the same slope, so there is no visible
/// corner where the rolloff starts -- a discontinuity in the derivative reads
/// as a hard edge across a sky.
///
/// The curve is rational rather than exponential, which is a correctness
/// choice and not a matter of taste: an exponential rolloff underflows to
/// exactly 1 about four and a half stops above the knee, and PQ footage arrives
/// with values up to 100 (Phase 6h), so the top two stops of an HDR signal
/// would have come out as flat white.
///
/// A knee of 1 or more means no rolloff at all: the encoder clips, which is
/// what this program did before there was a choice and what an SDR project with
/// nothing above white wants anyway.
[[nodiscard]] float rolloff(float linear, float knee);

/// Apply `rolloff` to an image in place, on straight colour.
///
/// Un-premultiplied first, like the grade: a compressive curve applied to
/// premultiplied values would make the result depend on how transparent the
/// pixel is, so a highlight would tone map differently in the middle of a
/// dissolve than either side of it.
void toneMap(RgbaImage& image, float knee);

}  // namespace zaro::render
