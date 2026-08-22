#pragma once

#include "zaro/core/time/RationalTime.h"

namespace zaro::model {

/// Which parts of a graphic's animation are protected from being stretched.
///
/// A title's animation is nearly always three things: it comes on, it sits
/// there, and it goes off. Trimming the clip should change how long it sits
/// there and nothing else -- but keyframes are glued to the picture (ADR-008),
/// so trimming the tail off a title cuts the exit animation in half, and
/// stretching the clip stretches the exit until it looks like a mistake.
///
/// Durations at each end, measured from the clip's own start and end. The
/// middle stretches to fill whatever is left, so the intro and the outro run at
/// the speed they were drawn at whatever the clip's length becomes.
struct ResponsiveTime {
    time::RationalTime intro;
    time::RationalTime outro;

    /// The clip's duration when the protection was set up.
    ///
    /// Stored rather than derived, because it is the length the keyframes were
    /// authored against -- the thing the stretch is relative to. Without it a
    /// clip trimmed twice would stretch relative to its already-stretched self
    /// and drift a little further each time.
    time::RationalTime authored;

    [[nodiscard]] bool isSet() const noexcept {
        return authored.toSecondsDouble() > 0.0 &&
               (intro.toSecondsDouble() > 0.0 || outro.toSecondsDouble() > 0.0);
    }

    friend bool operator==(const ResponsiveTime&, const ResponsiveTime&) = default;
};

}  // namespace zaro::model
