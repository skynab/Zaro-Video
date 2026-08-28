#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace zaro::time {

/// An exact rational number, always stored in lowest terms with a positive
/// denominator.
///
/// This type is the foundation of every temporal quantity in CutReel. Frame rates,
/// durations and positions are rationals -- never doubles. 29.97 is exactly
/// 30000/1001 and nothing else; representing it as a float is how an editor
/// accumulates a frame of drift over a long timeline and how audio slides out
/// of sync against picture.
///
/// Arithmetic reduces cross terms before multiplying, which keeps intermediate
/// values small enough that overflow is practically unreachable for the
/// magnitudes an editor deals in. Genuine overflow is a programming error and
/// trips an assertion in debug builds.
class Rational {
public:
    constexpr Rational() noexcept = default;

    /// Construct num/den, normalising sign and reducing to lowest terms.
    /// A zero denominator is a precondition violation, not a runtime error.
    Rational(std::int64_t num, std::int64_t den);

    /// Construct the integer `value` as value/1.
    static constexpr Rational fromInt(std::int64_t value) noexcept {
        return Rational{value, 1, PreNormalised{}};
    }

    /// Best rational approximation of `value` with a denominator no larger than
    /// `maxDenominator`, via continued fractions. Intended for parsing and for
    /// interop with float-based APIs -- not for use inside the time pipeline.
    static Rational approximate(double value, std::int64_t maxDenominator = 1000000);

    [[nodiscard]] constexpr std::int64_t num() const noexcept { return num_; }
    [[nodiscard]] constexpr std::int64_t den() const noexcept { return den_; }

    [[nodiscard]] constexpr bool isZero() const noexcept { return num_ == 0; }
    [[nodiscard]] constexpr bool isPositive() const noexcept { return num_ > 0; }
    [[nodiscard]] constexpr bool isNegative() const noexcept { return num_ < 0; }

    /// Lossy. For display, UI and interop only.
    [[nodiscard]] constexpr double toDouble() const noexcept {
        return static_cast<double>(num_) / static_cast<double>(den_);
    }

    /// Exact only when the denominator is 1.
    [[nodiscard]] constexpr bool isInteger() const noexcept { return den_ == 1; }

    [[nodiscard]] Rational inverse() const;
    [[nodiscard]] Rational abs() const;

    [[nodiscard]] std::int64_t floorToInt() const;
    [[nodiscard]] std::int64_t ceilToInt() const;
    /// Round to nearest; ties away from zero.
    [[nodiscard]] std::int64_t roundToInt() const;

    Rational& operator+=(const Rational& rhs);
    Rational& operator-=(const Rational& rhs);
    Rational& operator*=(const Rational& rhs);
    Rational& operator/=(const Rational& rhs);

    friend Rational operator+(Rational lhs, const Rational& rhs) { return lhs += rhs; }
    friend Rational operator-(Rational lhs, const Rational& rhs) { return lhs -= rhs; }
    friend Rational operator*(Rational lhs, const Rational& rhs) { return lhs *= rhs; }
    friend Rational operator/(Rational lhs, const Rational& rhs) { return lhs /= rhs; }

    [[nodiscard]] Rational operator-() const;

    friend bool operator==(const Rational& lhs, const Rational& rhs) noexcept {
        return lhs.num_ == rhs.num_ && lhs.den_ == rhs.den_;
    }
    friend std::strong_ordering operator<=>(const Rational& lhs, const Rational& rhs) noexcept;

    /// "30000/1001", or "24" when the denominator is 1.
    [[nodiscard]] std::string toString() const;

    /// Parses "num/den", a bare integer, or a decimal such as "23.976".
    /// Decimals are snapped to a known broadcast rate when they are within
    /// rounding distance of one, so "23.976" yields exactly 24000/1001.
    [[nodiscard]] static std::optional<Rational> parse(std::string_view text);

private:
    struct PreNormalised {};
    constexpr Rational(std::int64_t num, std::int64_t den, PreNormalised) noexcept
        : num_{num}, den_{den} {}

    std::int64_t num_{0};
    std::int64_t den_{1};
};

/// The frame rates a video application actually meets. Anything else is
/// expressible; these are named because they appear constantly.
namespace rates {
inline const Rational fps23_976{24000, 1001};
inline const Rational fps24{24, 1};
inline const Rational fps25{25, 1};
inline const Rational fps29_97{30000, 1001};
inline const Rational fps30{30, 1};
inline const Rational fps48{48, 1};
inline const Rational fps50{50, 1};
inline const Rational fps59_94{60000, 1001};
inline const Rational fps60{60, 1};
inline const Rational fps119_88{120000, 1001};
inline const Rational fps120{120, 1};

/// Common audio sample rates, expressed the same way.
inline const Rational hz44100{44100, 1};
inline const Rational hz48000{48000, 1};
inline const Rational hz96000{96000, 1};
}  // namespace rates

/// The integer rate a rate is "called": 30000/1001 is nominally 30. This is the
/// number timecode counts in, and it is never the same thing as the real rate.
[[nodiscard]] std::int64_t nominalRate(const Rational& rate);

}  // namespace zaro::time
