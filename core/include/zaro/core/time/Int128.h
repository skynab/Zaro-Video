#pragma once

#include <compare>
#include <cstdint>

namespace zaro::time::detail {

/// A signed 128-bit integer, for compilers that do not have one.
///
/// Rational arithmetic multiplies two int64 terms and needs the product exact
/// before it is reduced and narrowed back -- so a wider intermediate is not an
/// optimisation there, it is what makes an overflow detectable rather than
/// silent. GCC and Clang provide `__int128`; MSVC never has, and its 128-bit
/// intrinsics (`_mul128`, `_div128`) only cover a 128-by-64 divide whose
/// quotient fits in 64 bits -- which the gcd loop in Rational.cpp does not.
///
/// So this is the fallback, and Rational.cpp keeps using `__int128` wherever
/// the compiler has one. The two are meant to be indistinguishable: the same
/// two's-complement representation, the same truncate-toward-zero division,
/// the same sign-of-dividend remainder. test_int128.cpp checks exactly that,
/// operation by operation, against `__int128` on the platforms that have both
/// -- so the path Windows takes is covered by the platforms that do not take
/// it.
///
/// Deliberately not a general-purpose numeric type: it carries the operations
/// Rational needs and no others.
class Int128 {
public:
    constexpr Int128() noexcept = default;

    // Implicit, because the arithmetic in Rational.cpp reads as ordinary
    // arithmetic only if an int64 can stand in for an Int128 without ceremony.
    // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
    constexpr Int128(std::int64_t value) noexcept
        : lo_{static_cast<std::uint64_t>(value)},
          hi_{value < 0 ? ~std::uint64_t{0} : std::uint64_t{0}} {}

    [[nodiscard]] constexpr bool isNegative() const noexcept { return (hi_ >> 63U) != 0; }

    /// Explicit, and truncating: narrowing is the caller's decision, and
    /// Rational range-checks before it asks.
    explicit constexpr operator std::int64_t() const noexcept {
        return static_cast<std::int64_t>(lo_);
    }

    constexpr Int128 operator-() const noexcept {
        // Two's complement negation, which is also correct for the minimum.
        Int128 result;
        result.lo_ = ~lo_ + 1U;
        result.hi_ = ~hi_ + (result.lo_ == 0 ? 1U : 0U);
        return result;
    }

    constexpr Int128& operator+=(const Int128& rhs) noexcept {
        const std::uint64_t sum = lo_ + rhs.lo_;
        hi_ += rhs.hi_ + (sum < lo_ ? 1U : 0U);
        lo_ = sum;
        return *this;
    }

    constexpr Int128& operator-=(const Int128& rhs) noexcept { return *this += -rhs; }

    constexpr Int128& operator*=(const Int128& rhs) noexcept {
        // Wrapping, like the built-in type. A product of two int64s fits in
        // 128 bits, so for every multiplication Rational performs this is the
        // exact answer, sign and all: two's complement multiplication is the
        // same bit pattern whether the operands are read as signed or not.
        Int128 result = multiplyUnsigned(lo_, rhs.lo_);
        result.hi_ += (lo_ * rhs.hi_) + (hi_ * rhs.lo_);
        *this = result;
        return *this;
    }

    constexpr Int128& operator/=(const Int128& rhs) noexcept {
        Int128 quotient;
        Int128 remainder;
        divide(*this, rhs, quotient, remainder);
        *this = quotient;
        return *this;
    }

    constexpr Int128& operator%=(const Int128& rhs) noexcept {
        Int128 quotient;
        Int128 remainder;
        divide(*this, rhs, quotient, remainder);
        *this = remainder;
        return *this;
    }

    friend constexpr Int128 operator+(Int128 lhs, const Int128& rhs) noexcept { return lhs += rhs; }
    friend constexpr Int128 operator-(Int128 lhs, const Int128& rhs) noexcept { return lhs -= rhs; }
    friend constexpr Int128 operator*(Int128 lhs, const Int128& rhs) noexcept { return lhs *= rhs; }
    friend constexpr Int128 operator/(Int128 lhs, const Int128& rhs) noexcept { return lhs /= rhs; }
    friend constexpr Int128 operator%(Int128 lhs, const Int128& rhs) noexcept { return lhs %= rhs; }

    friend constexpr bool operator==(const Int128& lhs, const Int128& rhs) noexcept {
        return lhs.hi_ == rhs.hi_ && lhs.lo_ == rhs.lo_;
    }

    friend constexpr std::strong_ordering operator<=>(const Int128& lhs,
                                                      const Int128& rhs) noexcept {
        if (lhs.hi_ != rhs.hi_) {
            return static_cast<std::int64_t>(lhs.hi_) <=> static_cast<std::int64_t>(rhs.hi_);
        }
        return lhs.lo_ <=> rhs.lo_;
    }

private:
    constexpr Int128(std::uint64_t low, std::uint64_t high) noexcept : lo_{low}, hi_{high} {}

    /// 64x64 to 128, in 32-bit limbs so it needs nothing wider than it builds.
    [[nodiscard]] static constexpr Int128 multiplyUnsigned(std::uint64_t a,
                                                           std::uint64_t b) noexcept {
        constexpr std::uint64_t kMask = 0xFFFFFFFFU;
        const std::uint64_t aLow = a & kMask;
        const std::uint64_t aHigh = a >> 32U;
        const std::uint64_t bLow = b & kMask;
        const std::uint64_t bHigh = b >> 32U;

        const std::uint64_t lowLow = aLow * bLow;
        const std::uint64_t lowHigh = aLow * bHigh;
        const std::uint64_t highLow = aHigh * bLow;
        const std::uint64_t highHigh = aHigh * bHigh;

        const std::uint64_t middle = (lowLow >> 32U) + (lowHigh & kMask) + (highLow & kMask);
        const std::uint64_t low = (middle << 32U) | (lowLow & kMask);
        const std::uint64_t high = highHigh + (lowHigh >> 32U) + (highLow >> 32U) + (middle >> 32U);
        return Int128{low, high};
    }

    [[nodiscard]] constexpr bool isZero() const noexcept { return lo_ == 0 && hi_ == 0; }

    constexpr void shiftLeftOne() noexcept {
        hi_ = (hi_ << 1U) | (lo_ >> 63U);
        lo_ <<= 1U;
    }

    /// Unsigned compare, for the magnitudes inside divide().
    [[nodiscard]] static constexpr bool lessUnsigned(const Int128& lhs,
                                                     const Int128& rhs) noexcept {
        return lhs.hi_ != rhs.hi_ ? lhs.hi_ < rhs.hi_ : lhs.lo_ < rhs.lo_;
    }

    static constexpr void subtractUnsigned(Int128& lhs, const Int128& rhs) noexcept {
        const std::uint64_t difference = lhs.lo_ - rhs.lo_;
        lhs.hi_ -= rhs.hi_ + (lhs.lo_ < rhs.lo_ ? 1U : 0U);
        lhs.lo_ = difference;
    }

    /// Truncating toward zero, with the remainder taking the sign of the
    /// dividend -- the same contract as the built-in operators. Shift and
    /// subtract, 128 iterations, on a path that runs a handful of times per
    /// edit rather than per sample.
    ///
    /// A zero divisor yields zero rather than trapping. Every caller in
    /// Rational.cpp has already rejected one with ZARO_CHECK, and a constexpr
    /// function cannot divide by zero at compile time anyway.
    /// Both results come back through references because a nested struct
    /// holding two Int128s cannot be declared inside Int128's own definition.
    static constexpr void divide(const Int128& dividend, const Int128& divisor, Int128& quotientOut,
                                 Int128& remainderOut) noexcept {
        const bool negativeQuotient = dividend.isNegative() != divisor.isNegative();
        const bool negativeRemainder = dividend.isNegative();

        const Int128 top = dividend.isNegative() ? -dividend : dividend;
        const Int128 bottom = divisor.isNegative() ? -divisor : divisor;

        Int128 quotient;
        Int128 remainder;
        if (!bottom.isZero()) {
            for (int bit = 127; bit >= 0; --bit) {
                const bool inHigh = bit >= 64;
                const auto offset = static_cast<unsigned>(inHigh ? bit - 64 : bit);
                remainder.shiftLeftOne();
                remainder.lo_ |= ((inHigh ? top.hi_ : top.lo_) >> offset) & 1U;
                if (!lessUnsigned(remainder, bottom)) {
                    subtractUnsigned(remainder, bottom);
                    (inHigh ? quotient.hi_ : quotient.lo_) |= std::uint64_t{1} << offset;
                }
            }
        }
        quotientOut = negativeQuotient ? -quotient : quotient;
        remainderOut = negativeRemainder ? -remainder : remainder;
    }

    std::uint64_t lo_{0};
    std::uint64_t hi_{0};
};

}  // namespace zaro::time::detail
