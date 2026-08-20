#pragma once

#include <string>

#include "zaro/core/model/Animation.h"
#include "zaro/core/model/ClipEffects.h"
#include "zaro/core/model/ColorCorrection.h"
#include "zaro/core/model/Graphic.h"
#include "zaro/core/model/Ids.h"
#include "zaro/core/model/Secondary.h"
#include "zaro/core/model/ToneCurve.h"
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

    /// Primary colour correction, applied in the linear working space before
    /// the clip is placed in the frame.
    ColorCorrection color;

    /// A generated picture instead of a read one. When set, `source` is
    /// ignored: the clip has no media, and everything else about it -- trims,
    /// transforms, grades, keyframes, links -- works unchanged.
    Graphic graphic;

    /// A look LUT, applied after the primary correction and before the curves:
    /// a LUT is a look put on a balanced picture, and the curves are the
    /// adjustment made on top of the look.
    LutRef lut;

    /// Tone curves, applied after the primary correction. Curves are defined in
    /// the display-encoded domain; the baking into linear happens in
    /// render::CurveTable.
    ToneCurves curves;

    /// One secondary: a correction applied only where its qualifier selects.
    /// One rather than a list, for now — the machinery is the same either way,
    /// and a list with no UI to manage it is a list nobody can reach.
    Secondary secondary;

    /// Curves that override the static values above, where they exist.
    ClipAnimation animation;

    [[nodiscard]] const time::RationalTime& start() const { return timelineRange.start(); }
    [[nodiscard]] time::RationalTime endExclusive() const { return timelineRange.endExclusive(); }
    [[nodiscard]] const time::RationalTime& duration() const { return timelineRange.duration(); }

    /// The source time to read for a given timeline time, mapped linearly
    /// across the clip. Linear is exactly right at normal speed and is the hook
    /// a speed or time-remap curve replaces later.
    [[nodiscard]] time::RationalTime sourceTimeAt(const time::RationalTime& timelineTime) const;

    /// Where a source time sits on the timeline: the inverse of `sourceTimeAt`.
    ///
    /// Keyframes are stored in source time and drawn on the timeline, so this
    /// is the mapping the UI needs. It is the inverse rather than a second
    /// stored position, because a stored position would have to be maintained
    /// through every trim, and the first one missed would put a keyframe
    /// somewhere its curve does not agree with.
    [[nodiscard]] time::RationalTime timelineTimeOf(const time::RationalTime& sourceTime) const;

    /// The same mapping as `sourceTimeAt`, in seconds and unquantised.
    ///
    /// Animation is sampled at the sequence's rate, which need not be the
    /// source's. Rounding to a source frame first would make a 24fps clip on a
    /// 60fps timeline hold each animated value for two or three output frames,
    /// turning a smooth move into a stutter that no keyframe accounts for.
    [[nodiscard]] double sourceSecondsAt(const time::RationalTime& timelineTime) const;

    /// The transform to composite with at a moment, curves applied. Returns the
    /// static transform untouched when nothing is animated, which is the case
    /// for almost every clip.
    [[nodiscard]] Transform transformAt(const time::RationalTime& timelineTime) const;

    /// The correction at a moment, curves applied.
    [[nodiscard]] ColorCorrection colorAt(const time::RationalTime& timelineTime) const;

    /// The static value of one parameter, by name rather than by field.
    ///
    /// The keyframe operations and the parameter panel both need to treat
    /// parameters uniformly — the alternative is a ten-way switch repeated in
    /// every one of them, which is where a parameter gets forgotten.
    [[nodiscard]] double parameterValue(Param param) const;
    void setParameterValue(Param param, double value);

    /// The value to use at a moment: the curve if there is one, the static
    /// value if not.
    [[nodiscard]] double parameterAt(Param param, const time::RationalTime& timelineTime) const;

    [[nodiscard]] double gainDbAt(const time::RationalTime& timelineTime) const;
    [[nodiscard]] double panAt(const time::RationalTime& timelineTime) const;

    friend bool operator==(const Clip&, const Clip&) = default;
};

}  // namespace zaro::model
