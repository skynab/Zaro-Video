#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "zaro/core/model/Animation.h"

namespace zaro::model {

/// One of the effects a clip can carry.
///
/// An enum rather than a string, for the reason `Param` is one: a misspelling
/// should be a compile error rather than an effect that silently does nothing.
enum class EffectKind : std::uint8_t {
    Blur,
    Sharpen,
    /// Radial lens distortion: barrel out, pincushion in.
    Distort,
};

[[nodiscard]] const char* toString(EffectKind kind) noexcept;
[[nodiscard]] bool effectKindFromString(const char* name, EffectKind& out) noexcept;
[[nodiscard]] std::span<const EffectKind> allEffectKinds() noexcept;

/// A number an effect takes.
///
/// Shared across effects rather than named per effect: a radius means the same
/// thing to a blur and to a sharpen, and giving each its own name would mean
/// two controls, two serialized keys and two chances to disagree about units.
enum class EffectParam : std::uint8_t {
    /// The standard deviation of the Gaussian, in output pixels.
    Radius,
    /// How strongly the effect applies, 0 to 1 or beyond.
    Amount,
    /// How much the picture bends away from straight, as the coefficient of a
    /// radial term.
    ///
    /// Positive draws the picture in towards the centre, which is what
    /// straightens the bulge of a wide lens; negative pushes it out, which is
    /// how a pincushion is corrected or a fisheye is faked.
    Curvature,
    /// A scale about the centre, applied with the bend. Straightening a barrel
    /// leaves the corners empty, and this is what fills them back in -- kept as
    /// its own control rather than derived, because how much of the frame to
    /// give up is a decision about the shot.
    Zoom,
};

[[nodiscard]] const char* toString(EffectParam param) noexcept;
[[nodiscard]] bool effectParamFromString(const char* name, EffectParam& out) noexcept;

/// What one parameter of one effect means: its range, and where it starts.
///
/// A table rather than code, so that adding an effect is data and the panel
/// that shows it needs no new widgets -- and so that a default cannot be one
/// value in the model and another in the control.
struct EffectParamInfo {
    EffectParam param;
    double defaultValue;
    double minimum;
    double maximum;
    /// The step a control moves in, which is also a statement about how precise
    /// the number needs to be.
    double step;
};

/// The parameters an effect of this kind takes, in the order to show them.
[[nodiscard]] std::span<const EffectParamInfo> parametersOf(EffectKind kind) noexcept;

/// One effect on a clip.
struct Effect {
    EffectKind kind{EffectKind::Blur};

    /// Off without being removed, so a look can be compared against itself
    /// without losing the settings that produced it.
    bool enabled{true};

    /// Only the parameters that differ from their defaults are stored, so an
    /// effect somebody added and left alone costs one entry rather than a copy
    /// of the table.
    std::map<EffectParam, double> values;

    /// Curves that override the static values above, where they exist.
    ///
    /// Held on the effect rather than in the clip's `ClipAnimation`, and that
    /// is the whole design. A parameter inside a reorderable list has no stable
    /// name in a flat map: the obvious alternative is to give every effect an
    /// id and key the clip's animation by (id, parameter), which means two
    /// structures that have to be kept agreeing through every add, remove and
    /// reorder. Keeping the curves inside the effect makes all three free --
    /// moving an effect carries its animation, deleting one takes its curves,
    /// and copying a clip copies both together, with nothing to remember.
    std::map<EffectParam, Curve> animation;

    /// The static value, ignoring any curve.
    [[nodiscard]] double value(EffectParam param) const;
    void setValue(EffectParam param, double value) { values[param] = value; }

    /// The value at a moment in the clip's source time, in seconds -- the curve
    /// where there is one, the static value where there is not.
    [[nodiscard]] double valueAt(EffectParam param, double seconds) const;

    [[nodiscard]] bool isAnimated(EffectParam param) const;
    /// The curve for a parameter, or null.
    [[nodiscard]] const Curve* curve(EffectParam param) const;

    friend bool operator==(const Effect&, const Effect&) = default;
};

/// Whether any of them would change the picture at any moment.
///
/// Conservative about animation on purpose: an effect with a curve counts as
/// active even where the curve happens to read zero, because deciding
/// otherwise would mean the renderer taking one path on some frames of a ramp
/// and another on the rest.
[[nodiscard]] bool anyActive(const std::vector<Effect>& effects);

}  // namespace zaro::model
