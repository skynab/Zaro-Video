#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace zaro::model {

/// One of the effects a clip can carry.
///
/// An enum rather than a string, for the reason `Param` is one: a misspelling
/// should be a compile error rather than an effect that silently does nothing.
enum class EffectKind : std::uint8_t {
    Blur,
    Sharpen,
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

    [[nodiscard]] double value(EffectParam param) const;
    void setValue(EffectParam param, double value) { values[param] = value; }

    friend bool operator==(const Effect&, const Effect&) = default;
};

/// Whether any of them would change the picture.
[[nodiscard]] bool anyActive(const std::vector<Effect>& effects);

}  // namespace zaro::model
