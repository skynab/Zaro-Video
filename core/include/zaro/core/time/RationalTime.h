#pragma once

#include <compare>
#include <cstdint>
#include <string>

#include "zaro/core/time/Rational.h"

namespace zaro::time {

/// A point in time, or a duration, expressed as a whole number of frames at an
/// exact rate.
///
/// Two things make this the right representation for an editor rather than
/// "seconds as a double":
///
///  * Timeline positions are inherently discrete. A cut lands on a frame
///    boundary, never between two. Storing the frame index makes that
///    structural instead of a rounding convention applied after the fact.
///  * The rate travels with the value, so a source frame at 23.976 and a
///    timeline frame at 59.94 can never be silently confused for one another.
///
/// The same type serves for durations; a duration is just a frame count whose
/// origin is elsewhere.
class RationalTime {
public:
    constexpr RationalTime() noexcept = default;

    RationalTime(std::int64_t frames, Rational rate) : frames_{frames}, rate_{std::move(rate)} {}

    /// Nearest frame to an exact number of seconds, ties away from zero.
    [[nodiscard]] static RationalTime fromSeconds(const Rational& seconds, const Rational& rate);

    [[nodiscard]] constexpr std::int64_t frames() const noexcept { return frames_; }
    [[nodiscard]] const Rational& rate() const noexcept { return rate_; }

    /// Exact. frames / rate, never a float.
    [[nodiscard]] Rational toSeconds() const;

    /// Lossy. Display and interop only.
    [[nodiscard]] double toSecondsDouble() const { return toSeconds().toDouble(); }

    /// The same instant counted at a different rate, rounded to the nearest
    /// frame. Rescaling is lossy whenever the rates are not integer multiples,
    /// which is precisely why it has to be spelled out at the call site.
    [[nodiscard]] RationalTime rescaledTo(const Rational& newRate) const;
    [[nodiscard]] RationalTime rescaledTo(const RationalTime& other) const {
        return rescaledTo(other.rate());
    }

    [[nodiscard]] bool isZero() const noexcept { return frames_ == 0; }
    [[nodiscard]] RationalTime abs() const;

    /// Mixed-rate arithmetic resolves to the finer of the two rates, so the
    /// result never loses resolution the operands had. Same-rate arithmetic --
    /// which is the overwhelmingly common case -- is exact and allocation free.
    RationalTime& operator+=(const RationalTime& rhs);
    RationalTime& operator-=(const RationalTime& rhs);

    friend RationalTime operator+(RationalTime lhs, const RationalTime& rhs) { return lhs += rhs; }
    friend RationalTime operator-(RationalTime lhs, const RationalTime& rhs) { return lhs -= rhs; }
    [[nodiscard]] RationalTime operator-() const { return RationalTime{-frames_, rate_}; }

    /// Comparison is by instant, not by representation: 12@24 equals 24@48.
    friend bool operator==(const RationalTime& lhs, const RationalTime& rhs) noexcept;
    friend std::strong_ordering operator<=>(const RationalTime& lhs,
                                            const RationalTime& rhs) noexcept;

    /// "120@24000/1001"; diagnostics only.
    [[nodiscard]] std::string toString() const;

private:
    std::int64_t frames_{0};
    Rational rate_{rates::fps24};
};

}  // namespace zaro::time
