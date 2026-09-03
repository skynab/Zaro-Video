#pragma once

#include <functional>
#include <string>
#include <vector>

#include "zaro/core/Error.h"
#include "zaro/core/model/Ids.h"
#include "zaro/core/render/FrameSource.h"
#include "zaro/core/time/RationalTime.h"

namespace zaro::render {

struct StabiliseOptions {
    /// How much camera movement counts as intended.
    ///
    /// The smoothing window, in seconds. Shorter leaves the shake in; longer
    /// fights the pan as well, which is worse -- a stabiliser that flattens a
    /// deliberate move has taken the shot away from whoever framed it. Half a
    /// second is about where hand shake ends and intent begins.
    double smoothingSeconds{0.5};

    /// The tracking grid, per axis. Nine patches is enough that a subject
    /// walking through two of them does not carry the answer with it, and few
    /// enough to stay affordable on a long clip.
    int patchesPerAxis{3};
};

struct StabiliseResult {
    /// What to add to the clip's position, per frame, in source pixels. Same
    /// length as the times that were analysed.
    std::vector<double> x;
    std::vector<double> y;

    /// How far in to push the picture so the corrections never expose an edge.
    ///
    /// One number for the whole clip rather than a curve: a zoom that changed
    /// while the correction did would be a slow breathing that reads as a
    /// focus pull, and is much more noticeable than a slightly tighter frame.
    double zoom{1.0};

    /// Frames whose motion was measured. One less than the times analysed when
    /// everything worked, since motion is between frames.
    int measured{0};
    /// Set when measuring stopped early -- a shot change, or a frame with
    /// nothing trackable in it. Everything up to that point is still returned.
    std::string stopped;
};

/// Work out how to hold a shaky shot still.
///
/// **A grid of patches, reduced by the median.** One patch tracks whatever
/// happens to be under it, which on a real shot is as likely to be a person
/// walking as the background. Nine patches spread over the frame and the median
/// of what they say is the camera: a subject can dominate a few of them and
/// still not move the answer, and the median needs no threshold to tune, unlike
/// throwing outliers away.
///
/// **The camera path is integrated, then smoothed, and the difference is the
/// correction.** Smoothing the *motion* instead would leave the path free to
/// wander, which is exactly the low-frequency drift that makes stabilised
/// footage look like it is floating.
///
/// **Translation only**, as the tracker is: see `Tracker.h` for why one patch
/// cannot separate rotation and scale from it. A shot with roll in it comes out
/// better than it went in and not perfect.
///
/// `tell` is asked once per frame analysed, with how many are done and how many
/// there are, and stops the analysis when it answers false. Optional: the
/// offline callers pass nothing. It exists because a stabilise reads every
/// frame of the clip, and a caller with a window to keep alive needs both to
/// say so and to be able to stop.
using StabiliseProgress = std::function<bool(std::int64_t done, std::int64_t total)>;

[[nodiscard]] Result<StabiliseResult> stabilise(FrameSource& source, model::MediaRefId media,
                                                const std::vector<time::RationalTime>& sourceTimes,
                                                const StabiliseOptions& options = {},
                                                const StabiliseProgress& tell = {});

}  // namespace zaro::render
