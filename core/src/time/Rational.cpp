#include "zaro/core/time/Rational.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <numeric>

#include "zaro/core/Check.h"

namespace zaro::time {
namespace {

static_assert(sizeof(void*) >= 4, "unsupported target");

#if defined(__SIZEOF_INT128__)
// __extension__ keeps -Wpedantic quiet about the non-ISO type.
__extension__ using Wide = __int128;
#else
#error \
    "CutReel's rational arithmetic needs a 128-bit integer type. \
Port makeChecked() to a checked-64-bit path before enabling this target."
#endif

constexpr Wide wideAbs(Wide v) noexcept {
    return v < 0 ? -v : v;
}

Wide wideGcd(Wide a, Wide b) noexcept {
    a = wideAbs(a);
    b = wideAbs(b);
    while (b != 0) {
        const Wide t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/// Reduce a 128-bit fraction and narrow it back to a Rational. Reduction
/// happens before the range check, so results that fit are accepted even when
/// the unreduced intermediate did not.
Rational makeChecked(Wide n, Wide d) {
    ZARO_CHECK(d != 0, "rational with zero denominator");
    if (d < 0) {
        n = -n;
        d = -d;
    }
    const Wide g = wideGcd(n, d);
    if (g > 1) {
        n /= g;
        d /= g;
    }
    constexpr Wide kMax = static_cast<Wide>(std::numeric_limits<std::int64_t>::max());
    constexpr Wide kMin = static_cast<Wide>(std::numeric_limits<std::int64_t>::min());
    // Enforced in release too: a truncated numerator here is a wrong timestamp
    // that would propagate into edits and exports without ever looking wrong.
    ZARO_CHECK(n <= kMax && n >= kMin && d <= kMax, "rational arithmetic overflowed int64");
    return Rational{static_cast<std::int64_t>(n), static_cast<std::int64_t>(d)};
}

const std::array<Rational, 11>& knownRates() {
    static const std::array<Rational, 11> kRates{rates::fps23_976, rates::fps24,    rates::fps25,
                                                 rates::fps29_97,  rates::fps30,    rates::fps48,
                                                 rates::fps50,     rates::fps59_94, rates::fps60,
                                                 rates::fps119_88, rates::fps120};
    return kRates;
}

}  // namespace

Rational::Rational(std::int64_t num, std::int64_t den) {
    ZARO_CHECK(den != 0, "rational constructed with a zero denominator");
    if (den < 0) {
        ZARO_CHECK(num != std::numeric_limits<std::int64_t>::min() &&
                       den != std::numeric_limits<std::int64_t>::min(),
                   "rational sign normalisation would overflow");
        num = -num;
        den = -den;
    }
    const std::int64_t g = std::gcd(num, den);
    if (g > 1) {
        num /= g;
        den /= g;
    }
    num_ = num;
    den_ = den;
}

Rational Rational::approximate(double value, std::int64_t maxDenominator) {
    assert(maxDenominator > 0);
    if (!std::isfinite(value)) {
        return Rational{};
    }

    const bool negative = value < 0.0;
    double x = negative ? -value : value;

    // Stern-Brocot / continued fraction expansion: walk convergents until the
    // denominator would exceed the budget, then take the best of the last two.
    std::int64_t prevNum = 0, prevDen = 1;
    std::int64_t curNum = 1, curDen = 0;
    double remainder = x;

    for (int i = 0; i < 64; ++i) {
        const double wholeD = std::floor(remainder);
        if (wholeD > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            break;
        }
        const auto whole = static_cast<std::int64_t>(wholeD);

        const std::int64_t nextNum = whole * curNum + prevNum;
        const std::int64_t nextDen = whole * curDen + prevDen;
        if (nextDen > maxDenominator || nextDen < 0) {
            break;
        }
        prevNum = curNum;
        prevDen = curDen;
        curNum = nextNum;
        curDen = nextDen;

        const double frac = remainder - wholeD;
        if (frac < 1e-12) {
            break;
        }
        remainder = 1.0 / frac;
    }

    if (curDen == 0) {
        return Rational{};
    }
    const Rational result{negative ? -curNum : curNum, curDen};
    return result;
}

Rational Rational::inverse() const {
    assert(num_ != 0 && "inverse of zero");
    return Rational{den_, num_};
}

Rational Rational::abs() const {
    return num_ < 0 ? Rational{-num_, den_} : *this;
}

Rational Rational::operator-() const {
    return Rational{-num_, den_};
}

std::int64_t Rational::floorToInt() const {
    std::int64_t q = num_ / den_;
    if (num_ % den_ != 0 && num_ < 0) {
        --q;
    }
    return q;
}

std::int64_t Rational::ceilToInt() const {
    std::int64_t q = num_ / den_;
    if (num_ % den_ != 0 && num_ > 0) {
        ++q;
    }
    return q;
}

std::int64_t Rational::roundToInt() const {
    // Ties away from zero. Integer division truncates toward zero, which gives
    // floor for the positive case and ceil for the negative one -- exactly the
    // two halves of round-half-away.
    const Wide n2 = static_cast<Wide>(num_) * 2;
    const Wide d2 = static_cast<Wide>(den_) * 2;
    const Wide adjust = num_ >= 0 ? static_cast<Wide>(den_) : -static_cast<Wide>(den_);
    return static_cast<std::int64_t>((n2 + adjust) / d2);
}

Rational& Rational::operator+=(const Rational& rhs) {
    // Reduce the denominators against each other first so the products stay
    // small; this is what keeps everyday arithmetic nowhere near overflow.
    const std::int64_t g = std::gcd(den_, rhs.den_);
    const std::int64_t lhsScale = rhs.den_ / g;
    const std::int64_t rhsScale = den_ / g;
    const Wide n = static_cast<Wide>(num_) * lhsScale + static_cast<Wide>(rhs.num_) * rhsScale;
    const Wide d = static_cast<Wide>(den_) * lhsScale;
    *this = makeChecked(n, d);
    return *this;
}

Rational& Rational::operator-=(const Rational& rhs) {
    return *this += -rhs;
}

Rational& Rational::operator*=(const Rational& rhs) {
    const std::int64_t g1 = std::gcd(num_, rhs.den_);
    const std::int64_t g2 = std::gcd(rhs.num_, den_);
    const Wide n = static_cast<Wide>(num_ / g1) * (rhs.num_ / g2);
    const Wide d = static_cast<Wide>(den_ / g2) * (rhs.den_ / g1);
    *this = makeChecked(n, d);
    return *this;
}

Rational& Rational::operator/=(const Rational& rhs) {
    assert(rhs.num_ != 0 && "division by zero rational");
    return *this *= rhs.inverse();
}

std::strong_ordering operator<=>(const Rational& lhs, const Rational& rhs) noexcept {
    const Wide l = static_cast<Wide>(lhs.num_) * rhs.den_;
    const Wide r = static_cast<Wide>(rhs.num_) * lhs.den_;
    if (l < r) {
        return std::strong_ordering::less;
    }
    if (l > r) {
        return std::strong_ordering::greater;
    }
    return std::strong_ordering::equal;
}

std::string Rational::toString() const {
    if (den_ == 1) {
        return std::to_string(num_);
    }
    return std::to_string(num_) + "/" + std::to_string(den_);
}

std::optional<Rational> Rational::parse(std::string_view text) {
    // Trim surrounding whitespace.
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }

    const auto toInt = [](std::string_view s, std::int64_t& out) -> bool {
        if (s.empty()) {
            return false;
        }
        bool negative = false;
        std::size_t i = 0;
        if (s[0] == '+' || s[0] == '-') {
            negative = s[0] == '-';
            i = 1;
        }
        if (i >= s.size()) {
            return false;
        }
        std::int64_t value = 0;
        for (; i < s.size(); ++i) {
            const char c = s[i];
            if (c < '0' || c > '9') {
                return false;
            }
            const auto digit = static_cast<std::int64_t>(c - '0');
            if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
                return false;
            }
            value = value * 10 + digit;
        }
        out = negative ? -value : value;
        return true;
    };

    if (const auto slash = text.find('/'); slash != std::string_view::npos) {
        std::int64_t n = 0;
        std::int64_t d = 0;
        if (!toInt(text.substr(0, slash), n) || !toInt(text.substr(slash + 1), d) || d == 0) {
            return std::nullopt;
        }
        return Rational{n, d};
    }

    if (const auto dot = text.find('.'); dot != std::string_view::npos) {
        const std::string_view wholePart = text.substr(0, dot);
        const std::string_view fracPart = text.substr(dot + 1);
        if (fracPart.empty() || fracPart.size() > 18) {
            return std::nullopt;
        }
        std::int64_t whole = 0;
        if (!wholePart.empty() && wholePart != "-" && wholePart != "+" &&
            !toInt(wholePart, whole)) {
            return std::nullopt;
        }
        std::int64_t frac = 0;
        if (!toInt(fracPart, frac) || frac < 0) {
            return std::nullopt;
        }
        std::int64_t scale = 1;
        for (std::size_t i = 0; i < fracPart.size(); ++i) {
            scale *= 10;
        }
        const bool negative = !wholePart.empty() && wholePart.front() == '-';
        Rational exact = Rational{whole, 1} + Rational{negative ? -frac : frac, scale};

        // "23.976" and "29.97" are how humans write 24000/1001 and 30000/1001.
        // Snap to the exact broadcast rate rather than honouring the typo.
        const Rational tolerance{1, 1000};
        for (const Rational& rate : knownRates()) {
            if ((exact - rate).abs() < tolerance) {
                return rate;
            }
        }
        return exact;
    }

    std::int64_t whole = 0;
    if (!toInt(text, whole)) {
        return std::nullopt;
    }
    return Rational::fromInt(whole);
}

std::int64_t nominalRate(const Rational& rate) {
    return rate.roundToInt();
}

}  // namespace zaro::time
