#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "zaro/core/time/RationalTime.h"

namespace zaro::model {

/// How the segment leaving a keyframe behaves.
///
/// The mode belongs to the *outgoing* keyframe rather than to a segment,
/// because that is how it is edited: you select a keyframe and say what
/// happens after it. A segment's shape still depends on both ends — a bezier
/// needs a handle at each — but only one of them decides whether there is a
/// curve at all.
enum class Interpolation : std::uint8_t {
    /// The value does not change until the next keyframe, then jumps. This is
    /// what a parameter that should switch rather than slide wants.
    Hold,
    Linear,
    Bezier,
};

[[nodiscard]] const char* toString(Interpolation interpolation) noexcept;
[[nodiscard]] Interpolation interpolationFromString(const char* name) noexcept;

/// A bezier control handle, as an offset from the keyframe it belongs to.
///
/// `dx` is a fraction of the segment it points into, not a duration. A handle
/// measured in seconds would change shape whenever the keyframes either side
/// of it moved, so dragging one keyframe would silently redraw the curve
/// leaving its neighbour. As a fraction, the shape survives retiming.
///
/// `dy` is in the parameter's own units, and is signed the way the handle
/// points: an outgoing handle adds, an incoming handle subtracts, so a
/// symmetric pair of handles describes a symmetric curve.
struct Handle {
    /// A third of the segment with no vertical offset is the classic ease, and
    /// a pair of these is exactly the curve people expect from "smooth".
    double dx{1.0 / 3.0};
    double dy{0.0};

    friend bool operator==(const Handle&, const Handle&) = default;
};

/// One value at one time.
///
/// The time is in the clip's *source* time, not sequence time. Keyframes
/// describe something happening to the picture, so they have to stay glued to
/// the picture: stored in sequence time, animation would be left behind by a
/// ripple and would slide against the frames whenever the clip's in point was
/// trimmed. In source time, a clip can be moved, trimmed, split and rejoined
/// and the fade still lands on the frame it was set on.
struct Keyframe {
    time::RationalTime time;
    double value{0.0};
    Interpolation interpolation{Interpolation::Linear};
    /// The handle leaving this keyframe, used when this keyframe is a bezier.
    Handle out;
    /// The handle arriving at this keyframe, used when the *previous* keyframe
    /// is a bezier.
    Handle in;

    friend bool operator==(const Keyframe&, const Keyframe&) = default;
};

/// An animated scalar: keyframes ordered in time, and the rule for reading a
/// value between them.
///
/// Everything animatable in this application is a scalar double. A position is
/// two curves rather than one curve of pairs, which is what lets the horizontal
/// and vertical components ease differently — the thing that makes an arc look
/// like an arc rather than a diagonal.
class Curve {
public:
    [[nodiscard]] bool empty() const noexcept { return keys_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return keys_.size(); }
    [[nodiscard]] const std::vector<Keyframe>& keyframes() const noexcept { return keys_; }

    /// Add a keyframe, or replace the one already at that time.
    ///
    /// Replacing rather than appending keeps the "one value per instant"
    /// invariant that evaluation depends on: two keyframes at the same time
    /// describe a zero-length segment, which has no answer.
    void set(const Keyframe& keyframe);

    /// Remove the keyframe at exactly this time. Returns whether there was one.
    bool removeAt(const time::RationalTime& time);

    [[nodiscard]] const Keyframe* at(const time::RationalTime& time) const;

    /// The value at a moment in source time, in seconds.
    ///
    /// Seconds rather than frames because the sequence and the source need not
    /// share a rate. A 24fps clip on a 60fps timeline evaluated in source
    /// frames would step, holding each value for two or three output frames and
    /// turning a smooth move into a stutter that no keyframe is responsible for.
    ///
    /// Outside the keyframed range the value is held, never extrapolated: the
    /// slope leaving the last keyframe is a statement about the segment before
    /// it, and continuing it produces opacities of nine and scales of minus two
    /// a few seconds later.
    [[nodiscard]] double valueAtSeconds(double seconds) const;

    /// The value for a curve that may be empty, falling back to the clip's
    /// static value. An empty curve is not "zero everywhere", it is "not
    /// animated", and those differ for every parameter whose neutral value is
    /// not zero.
    [[nodiscard]] static double valueOr(const Curve* curve, double seconds, double fallback) {
        return curve == nullptr || curve->empty() ? fallback : curve->valueAtSeconds(seconds);
    }

    friend bool operator==(const Curve&, const Curve&) = default;

private:
    /// Sorted by time, with no duplicates.
    std::vector<Keyframe> keys_;
};

/// Which property a curve drives.
///
/// An enum rather than a string means a misspelling is a compile error instead
/// of an animation that silently does nothing.
enum class Param : std::uint8_t {
    PositionX,
    PositionY,
    ScaleX,
    ScaleY,
    RotationDegrees,
    AnchorX,
    AnchorY,
    Opacity,
    GainDb,
    Pan,
    Temperature,
    Tint,
    Exposure,
    Contrast,
    Saturation,

    /// Which frame of the media is shown, in seconds of source time.
    ///
    /// The odd one out, and deliberately so. Every other parameter answers
    /// "what is done to the picture"; this one answers "which picture", which
    /// is why it is read before the others rather than alongside them, and why
    /// its own keyframes are the only ones positioned in the clip's
    /// *un-remapped* source time -- they are what defines the remapped one.
    TimeRemap,
};

/// Every parameter, once. Anything that has to visit them all uses this, so
/// adding a parameter is one edit rather than a hunt for hand-written lists
/// that each have to be found and updated.
[[nodiscard]] std::span<const Param> allParams() noexcept;

[[nodiscard]] const char* toString(Param param) noexcept;
/// The parameter of that name, or nothing if it is not one. Unknown names come
/// from project files written by a later version, and dropping one quietly is
/// better than refusing to open the file.
[[nodiscard]] bool paramFromString(const char* name, Param& out) noexcept;

/// Every curve on one clip.
///
/// Held apart from `Transform` rather than replacing its fields with animated
/// ones. A parameter that is not animated should cost nothing to store, nothing
/// to serialize and nothing to evaluate, and the overwhelming majority never
/// are. The static value stays authoritative until a curve exists for it, at
/// which point the curve wins outright.
class ClipAnimation {
public:
    [[nodiscard]] bool empty() const noexcept { return curves_.empty(); }
    [[nodiscard]] const Curve* find(Param param) const;
    /// The curve for a parameter, creating an empty one if there is none.
    Curve& curve(Param param) { return curves_[param]; }
    void erase(Param param) { curves_.erase(param); }

    [[nodiscard]] auto begin() const { return curves_.begin(); }
    [[nodiscard]] auto end() const { return curves_.end(); }

    /// Drop curves that have no keyframes left, so "is anything animated?" and
    /// "is anything stored?" cannot disagree.
    void pruneEmpty();

    friend bool operator==(const ClipAnimation&, const ClipAnimation&) = default;

private:
    std::map<Param, Curve> curves_;
};

}  // namespace zaro::model
