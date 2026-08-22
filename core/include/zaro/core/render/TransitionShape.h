#pragma once

#include "zaro/core/model/ClipEffects.h"
#include "zaro/core/model/Mask.h"
#include "zaro/core/model/Transition.h"

namespace zaro::render {

/// How the incoming clip is drawn part way through a transition.
///
/// One function, called by both render paths. The alternative -- each path
/// working out where a wipe's edge is -- is two answers to one question, and
/// this project has already paid for that once: the outgoing half of a
/// transition went two phases ungraded because three draw sites had drifted.
struct TransitionShape {
    /// Multiplies whatever the incoming clip's own opacity is. A dissolve is
    /// entirely this; a wipe and a slide leave it at 1.
    double opacity{1.0};

    /// Added to the incoming clip's own transform. A slide is entirely this.
    double offsetX{0.0};
    double offsetY{0.0};

    /// Where the incoming clip shows, in output coordinates -- the same space
    /// a clip's own mask lives in. A wipe is entirely this.
    ///
    /// `isSet()` is false for a dissolve and a slide. It does not replace the
    /// clip's own mask: both apply, and their coverages multiply, because a
    /// masked clip in a wipe should be shown where the mask says *and* where
    /// the wipe has got to.
    model::Mask wipe;
};

/// Work out the shape at a moment, given the frame it is drawn into.
[[nodiscard]] TransitionShape transitionShapeFor(const model::Transition& transition,
                                                 double progress, std::int32_t width,
                                                 std::int32_t height);

}  // namespace zaro::render
