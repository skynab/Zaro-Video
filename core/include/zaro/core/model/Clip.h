#pragma once

#include <string>

#include "zaro/core/model/ClipEffects.h"
#include "zaro/core/model/Ids.h"
#include "zaro/core/time/TimeRange.h"

namespace zaro::model {

/// One piece of media placed on a track.
///
/// A clip owns no pixels and no samples. It is a reference to a range of a
/// source plus a range of the timeline, which is what makes editing
/// non-destructive: every operation here rewrites two small time ranges and
/// nothing else.
///
/// The two ranges are kept independently rather than deriving one from the
/// other. A 23.976 source on a 25fps sequence has no exact frame
/// correspondence, and rate-converted or speed-changed material has none at
/// all, so the timeline range is authoritative for *where* and the source range
/// is authoritative for *what*. `sourceTimeAt` maps between them.
struct Clip {
    ClipId id;
    MediaRefId source;

    /// What to read, in the source's own frame rate.
    time::TimeRange sourceRange;
    /// Where it sits, in the sequence's frame rate.
    time::TimeRange timelineRange;

    std::string name;
    bool enabled{true};

    /// Which link group this clip belongs to, if any.
    ///
    /// Picture and its sound arrive together and should move together: dragging
    /// one and leaving the other behind is how a cut goes out of sync without
    /// anyone noticing. Clips sharing a link are moved, trimmed and removed as
    /// one; an invalid id means the clip stands alone.
    LinkId link;

    /// How the compositor places this clip. Ignored for audio clips.
    Transform transform;
    BlendMode blend{BlendMode::Normal};

    /// Clip gain in decibels, and pan from -1 (left) to +1 (right). Ignored for
    /// video clips. Decibels rather than a linear factor because that is the
    /// unit the value is edited and displayed in, and converting at the edges
    /// keeps rounding out of the stored value.
    double gainDb{0.0};
    double pan{0.0};

    [[nodiscard]] const time::RationalTime& start() const { return timelineRange.start(); }
    [[nodiscard]] time::RationalTime endExclusive() const { return timelineRange.endExclusive(); }
    [[nodiscard]] const time::RationalTime& duration() const { return timelineRange.duration(); }

    /// The source time to read for a given timeline time, mapped linearly
    /// across the clip. Linear is exactly right at normal speed and is the hook
    /// a speed or time-remap curve replaces later.
    [[nodiscard]] time::RationalTime sourceTimeAt(const time::RationalTime& timelineTime) const;

    friend bool operator==(const Clip&, const Clip&) = default;
};

}  // namespace zaro::model
