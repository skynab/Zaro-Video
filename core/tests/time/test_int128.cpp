#include <array>
#include <cstdint>
#include <limits>
#include <random>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/time/Int128.h"

using zaro::time::detail::Int128;

namespace {

constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();

/// Values worth trying against every operator: the ends of the range, the
/// signs, and a few ordinary numbers.
const std::array<std::int64_t, 13> kInteresting{
    0, 1, -1, 2, -2, 7, -7, 1000, -1000, 1'000'000'007, -1'000'000'007, kInt64Max, kInt64Min,
};

}  // namespace

// Every operation Rational asks of a 128-bit type, checked against the
// compiler's own -- which is the type this one stands in for on MSVC. Windows
// runs these too; there they check the fallback against itself, which still
// catches a change of behaviour, and it is macOS and Linux that hold it to
// __int128.
#if defined(__SIZEOF_INT128__)

namespace {

__extension__ using Reference = __int128;
__extension__ using Unsigned = unsigned __int128;

/// Rebuild a reference value as an Int128, through the public interface only:
/// 32 bits at a time, so no step needs a wider type than Int128 provides.
Int128 asInt128(Reference value) {
    const bool negative = value < 0;
    const Unsigned magnitude =
        negative ? ~static_cast<Unsigned>(value) + 1U : static_cast<Unsigned>(value);

    const Int128 shift{std::int64_t{1} << 32};
    Int128 result{};
    for (int chunk = 3; chunk >= 0; --chunk) {
        const auto part = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(magnitude >> (32 * chunk)) & 0xFFFFFFFFU);
        result = result * shift + Int128{part};
    }
    return negative ? -result : result;
}

/// The pairs both implementations are asked about: the interesting values in
/// every combination, then a deterministic spread of random ones.
std::vector<std::pair<std::int64_t, std::int64_t>> pairs() {
    std::vector<std::pair<std::int64_t, std::int64_t>> out;
    for (const std::int64_t a : kInteresting) {
        for (const std::int64_t b : kInteresting) {
            out.emplace_back(a, b);
        }
    }
    std::mt19937_64 rng{20240828};
    std::uniform_int_distribution<std::int64_t> any{kInt64Min, kInt64Max};
    for (int i = 0; i < 2000; ++i) {
        out.emplace_back(any(rng), any(rng));
    }
    return out;
}

}  // namespace

TEST_CASE("Int128 multiplies exactly, like __int128") {
    for (const auto& [a, b] : pairs()) {
        const Reference expected = static_cast<Reference>(a) * static_cast<Reference>(b);
        const Int128 actual = Int128{a} * Int128{b};
        INFO(a << " * " << b);
        CHECK(actual == asInt128(expected));
    }
}

TEST_CASE("Int128 adds, subtracts and negates like __int128") {
    for (const auto& [a, b] : pairs()) {
        // Sums of products, which is the shape operator+= gives makeChecked.
        const Reference left = static_cast<Reference>(a) * 3;
        const Reference right = static_cast<Reference>(b) * 5;
        INFO(a << " and " << b);
        CHECK(Int128{a} * Int128{3} + Int128{b} * Int128{5} == asInt128(left + right));
        CHECK(Int128{a} * Int128{3} - Int128{b} * Int128{5} == asInt128(left - right));
        CHECK(-(Int128{a} * Int128{3}) == asInt128(-left));
    }
}

TEST_CASE("Int128 divides toward zero and keeps the dividend's remainder sign") {
    for (const auto& [a, b] : pairs()) {
        if (b == 0) {
            continue;
        }
        // A wide dividend, so this exercises the full 128-bit path rather than
        // one that would have fitted in 64 bits.
        const Reference dividend = static_cast<Reference>(a) * static_cast<Reference>(kInt64Max);
        const auto divisor = static_cast<Reference>(b);
        const Int128 wide = Int128{a} * Int128{kInt64Max};
        INFO(a << " * int64max / " << b);
        CHECK(wide / Int128{b} == asInt128(dividend / divisor));
        CHECK(wide % Int128{b} == asInt128(dividend % divisor));
    }
}

TEST_CASE("Int128 orders values like __int128") {
    for (const auto& [a, b] : pairs()) {
        const Reference left = static_cast<Reference>(a) * static_cast<Reference>(kInt64Max);
        const Reference right = static_cast<Reference>(b) * static_cast<Reference>(kInt64Max);
        const Int128 wideLeft = Int128{a} * Int128{kInt64Max};
        const Int128 wideRight = Int128{b} * Int128{kInt64Max};
        INFO(a << " against " << b);
        CHECK((wideLeft < wideRight) == (left < right));
        CHECK((wideLeft > wideRight) == (left > right));
        CHECK((wideLeft == wideRight) == (left == right));
        CHECK((wideLeft <= wideRight) == (left <= right));
    }
}

#endif  // __SIZEOF_INT128__

// These hold on every platform, including the one that has no __int128 to
// compare against.

TEST_CASE("Int128 holds a product that overflows int64") {
    // The largest magnitude a product of two int64s can reach, which is where
    // a 64-bit intermediate would wrap and Rational would accept a wrong
    // timestamp instead of rejecting it.
    const Int128 squared = Int128{kInt64Min} * Int128{kInt64Min};
    CHECK(squared / Int128{kInt64Min} == Int128{kInt64Min});
    CHECK(squared % Int128{kInt64Min} == Int128{0});
    CHECK(squared > Int128{kInt64Max});
    CHECK_FALSE(squared.isNegative());

    const Int128 negative = Int128{kInt64Max} * Int128{kInt64Min};
    CHECK(negative.isNegative());
    CHECK(negative / Int128{kInt64Max} == Int128{kInt64Min});
}

TEST_CASE("Int128 truncates toward zero") {
    CHECK(Int128{7} / Int128{2} == Int128{3});
    CHECK(Int128{-7} / Int128{2} == Int128{-3});
    CHECK(Int128{7} / Int128{-2} == Int128{-3});
    CHECK(Int128{-7} / Int128{-2} == Int128{3});

    CHECK(Int128{7} % Int128{2} == Int128{1});
    CHECK(Int128{-7} % Int128{2} == Int128{-1});
    CHECK(Int128{7} % Int128{-2} == Int128{1});
    CHECK(Int128{-7} % Int128{-2} == Int128{-1});
}

TEST_CASE("Int128 narrows back to int64 when the value fits") {
    CHECK(static_cast<std::int64_t>(Int128{kInt64Max}) == kInt64Max);
    CHECK(static_cast<std::int64_t>(Int128{kInt64Min}) == kInt64Min);
    CHECK(static_cast<std::int64_t>(Int128{-1}) == -1);

    // Reduction before narrowing is the whole point: a product that does not
    // fit comes back to one that does.
    const Int128 product = Int128{kInt64Max} * Int128{4};
    CHECK(static_cast<std::int64_t>(product / Int128{4}) == kInt64Max);
}

TEST_CASE("Int128 compares across the sign boundary") {
    CHECK(Int128{-1} < Int128{0});
    CHECK(Int128{kInt64Min} < Int128{kInt64Max});
    CHECK(Int128{kInt64Min} * Int128{2} < Int128{kInt64Min});
    CHECK(Int128{kInt64Max} * Int128{2} > Int128{kInt64Max});
    CHECK(Int128{0} == Int128{0});
    CHECK(-Int128{0} == Int128{0});
}
