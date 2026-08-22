#pragma once

#include <string>

#include "zaro/core/model/Animation.h"
#include "zaro/core/model/AudioRole.h"
#include "zaro/core/model/ClipEffects.h"
#include "zaro/core/model/ColorCorrection.h"
#include "zaro/core/model/Effect.h"
#include "zaro/core/model/Graphic.h"
#include "zaro/core/model/Ids.h"
#include "zaro/core/model/Keyer.h"
#include "zaro/core/model/Mask.h"
#include "zaro/core/model/Responsive.h"
#include "zaro/core/model/Secondary.h"
#include "zaro/core/model/ToneCurve.h"
#include "zaro/core/model/Vignette.h"
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

    /// What this sound is for. Drives nothing on its own -- it is what the
    /// automatic decisions read, and what a mix is organised by.
    AudioRole role{AudioRole::Unassigned};
    double pan{0.0};

    /// Primary colour correction, applied in the linear working space before
    /// the clip is placed in the frame.
    ColorCorrection color;

    /// The three wheels, as an ASC CDL. Separate from `color` because it is a
    /// different, interchangeable description of a grade rather than more of
    /// the same one.
    ColorWheels wheels;

    /// A clip that grades everything beneath it instead of drawing anything.
    ///
    /// It has no picture of its own: its grade, curves, secondary, LUT, mask
    /// and opacity apply to whatever the tracks below have already composited.
    /// That is the whole feature -- one correction over a run of shots, held in
    /// one place, rather than pasted onto each of them and re-pasted whenever
    /// it changes.
    bool adjustment{false};

    /// One camera in a multicam clip.
    ///
    /// The offset is what syncs it: angles rarely start rolling together, so
    /// each carries how far into its own material the group's zero point is.
    /// Storing an offset rather than trimming each angle to a common start
    /// means switching angles never has to re-derive the sync -- which is the
    /// thing that must not drift, because a switch that lands a frame out is
    /// visible and a switch that lands a frame out *sometimes* is unfindable.
    struct Angle {
        MediaRefId media;
        time::RationalTime offset;
        std::string name;

        friend bool operator==(const Angle&, const Angle&) = default;
    };

    /// The cameras, when this is a multicam clip. Empty for an ordinary one.
    ///
    /// A clip rather than a container of clips: an angle switch is a cut, and a
    /// cut is something this timeline already does. Everything else -- trims,
    /// grades, transitions, keyframes -- goes on working because nothing about
    /// the clip changed except which file it reads.
    std::vector<Angle> angles;
    /// Which angle is live. Out of range means the first.
    std::int32_t activeAngle{0};

    [[nodiscard]] bool isMulticam() const noexcept { return !angles.empty(); }

    /// The media this clip actually reads, and the time in it.
    [[nodiscard]] MediaRefId activeSource() const;
    [[nodiscard]] time::RationalTime activeSourceTimeAt(
        const time::RationalTime& timelineTime) const;

    /// Another sequence, used as a clip.
    ///
    /// When set, `source` is ignored and the clip is whatever that sequence
    /// composites to. Its `sourceRange` is a range of the nested sequence's own
    /// timeline, which means every trim, retime and reverse already works on a
    /// nested clip without knowing it is one.
    SequenceId nested;

    /// A generated picture instead of a read one. When set, `source` is
    /// ignored: the clip has no media, and everything else about it -- trims,
    /// transforms, grades, keyframes, links -- works unchanged.
    Graphic graphic;

    /// Play the clip backwards.
    ///
    /// A flag rather than a negative speed, because speed is not stored: it is
    /// the ratio between the two ranges, which every trim and every retime
    /// already maintains. Adding a `speed` field would be a second source of
    /// truth about timing, and the first time the two disagreed the clip would
    /// play at one rate and be laid out at another. Direction is the one thing
    /// two positive ranges cannot express, so it is the one thing stored.
    bool reversed{false};

    /// How fast this clip plays, derived from its ranges. 2 is twice as fast,
    /// 0.5 half. Always positive; `reversed` says which way.
    [[nodiscard]] double speed() const;

    /// Where on the screen this clip shows through. In output coordinates, so
    /// it stays put when the clip moves.
    Mask mask;

    /// Darkening towards the corners, in output coordinates like the mask.
    Vignette vignette;

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

    /// Effects, in the order they are applied.
    ///
    /// A list rather than a field each, because order is the thing a list has
    /// and a set of fields does not: blurring and then sharpening is not the
    /// same picture as sharpening and then blurring, and somebody has to be
    /// able to say which they meant.
    ///
    /// These run on the clip's image, before the key and the grade. They are
    /// the only stage that reads a pixel's *neighbours*, so they cannot live in
    /// the per-pixel path the rest of the pipeline is; putting the spatial
    /// stage first is what keeps the rest a single pass.
    std::vector<Effect> effects;

    /// What of this clip is transparent, so a clip under it shows through.
    ///
    /// Read before the grade: a key measures what the camera saw, and running
    /// it afterwards would mean every adjustment to the look silently moved the
    /// edges of the matte.
    Keyer keyer;

    /// Curves that override the static values above, where they exist.
    ClipAnimation animation;

    /// Which parts of that animation survive a trim unstretched. See
    /// `ResponsiveTime`; empty means the animation stretches with the clip,
    /// which is what everything but a title wants.
    ResponsiveTime responsive;

    [[nodiscard]] const time::RationalTime& start() const { return timelineRange.start(); }
    [[nodiscard]] time::RationalTime endExclusive() const { return timelineRange.endExclusive(); }
    [[nodiscard]] const time::RationalTime& duration() const { return timelineRange.duration(); }

    /// The source time to read for a given timeline time, mapped linearly
    /// across the clip. Linear is exactly right at normal speed and is the hook
    /// a speed or time-remap curve replaces later.
    [[nodiscard]] time::RationalTime sourceTimeAt(const time::RationalTime& timelineTime) const;

    /// The same mapping with time remapping *not* applied: where the clip's
    /// trims, speed and direction alone say to read.
    ///
    /// This is what the audio reads, and what keyframes are positioned in.
    /// Audio because retiming a signal is resampling it and a remap changes
    /// rate continuously -- a problem worth solving on its own rather than
    /// badly in passing. Keyframes because they are placed against this
    /// mapping, and reading them through the remap they define would be
    /// circular.
    [[nodiscard]] time::RationalTime baseSourceTimeAt(const time::RationalTime& timelineTime) const;
    [[nodiscard]] time::RationalTime activeBaseSourceTimeAt(
        const time::RationalTime& timelineTime) const;

    /// Whether the clip picks its frames from a curve rather than from its
    /// range and speed.
    [[nodiscard]] bool isTimeRemapped() const;

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

    /// Where in the *authored* animation a moment falls: `sourceSecondsAt`
    /// with the responsive intro and outro applied.
    ///
    /// Every curve on a clip is read through this rather than through
    /// `sourceSecondsAt` directly -- except the time remap, which defines the
    /// mapping the others are read at and cannot be read through its own
    /// output.
    [[nodiscard]] double animationSecondsAt(const time::RationalTime& timelineTime) const;

    /// The transform to composite with at a moment, curves applied. Returns the
    /// static transform untouched when nothing is animated, which is the case
    /// for almost every clip.
    [[nodiscard]] Transform transformAt(const time::RationalTime& timelineTime) const;

    /// The correction at a moment, curves applied.
    [[nodiscard]] ColorCorrection colorAt(const time::RationalTime& timelineTime) const;

    /// The mask at a moment: the drawn one, moved by whatever the mask offset
    /// curves say. A shape moves its centre and a path moves every point, so
    /// the offset means the same thing either way.
    [[nodiscard]] Mask maskAt(const time::RationalTime& timelineTime) const;

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
