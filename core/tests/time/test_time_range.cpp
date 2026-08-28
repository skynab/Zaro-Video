#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/time/TimeRange.h"

using zaro::time::RationalTime;
using zaro::time::TimeRange;
namespace rates = zaro::time::rates;

namespace {
RationalTime f(std::int64_t frames) {
    return RationalTime{frames, rates::fps25};
}
TimeRange range(std::int64_t start, std::int64_t duration) {
    return TimeRange{f(start), f(duration)};
}
}  // namespace

TEST_CASE("TimeRange is half open", "[time][timerange]") {
    const TimeRange r = range(10, 5);  // frames 10,11,12,13,14

    CHECK(r.start() == f(10));
    CHECK(r.duration() == f(5));
    CHECK(r.endExclusive() == f(15));
    CHECK(r.endInclusive() == f(14));

    CHECK(r.contains(f(10)));
    CHECK(r.contains(f(14)));
    CHECK_FALSE(r.contains(f(15)));
    CHECK_FALSE(r.contains(f(9)));
}

TEST_CASE("Butt-jointed clips tile without gap or overlap", "[time][timerange]") {
    // This is the property the whole half-open convention exists to guarantee.
    const TimeRange a = range(0, 100);
    const TimeRange b = range(100, 100);

    CHECK(a.endExclusive() == b.start());
    CHECK_FALSE(a.overlaps(b));
    CHECK(a.meets(b));
    CHECK_FALSE(a.intersection(b).has_value());

    // Every frame belongs to exactly one of them.
    for (std::int64_t i = 0; i < 200; ++i) {
        CHECK(a.contains(f(i)) != b.contains(f(i)));
    }
}

TEST_CASE("TimeRange::fromStartEnd", "[time][timerange]") {
    CHECK(TimeRange::fromStartEnd(f(10), f(15)) == range(10, 5));

    SECTION("an end at or before the start yields an empty range") {
        CHECK(TimeRange::fromStartEnd(f(10), f(10)).isEmpty());
        CHECK(TimeRange::fromStartEnd(f(10), f(5)).isEmpty());
    }
}

TEST_CASE("TimeRange containment of other ranges", "[time][timerange]") {
    const TimeRange outer = range(0, 100);

    CHECK(outer.contains(range(0, 100)));
    CHECK(outer.contains(range(10, 10)));
    CHECK_FALSE(outer.contains(range(90, 20)));
    CHECK_FALSE(outer.contains(range(100, 1)));
}

TEST_CASE("TimeRange overlap and intersection", "[time][timerange]") {
    SECTION("partial overlap") {
        const auto hit = range(0, 100).intersection(range(50, 100));
        REQUIRE(hit.has_value());
        CHECK(*hit == range(50, 50));
    }

    SECTION("containment") {
        const auto hit = range(0, 100).intersection(range(20, 30));
        REQUIRE(hit.has_value());
        CHECK(*hit == range(20, 30));
    }

    SECTION("disjoint") {
        CHECK_FALSE(range(0, 10).intersection(range(20, 10)).has_value());
    }

    SECTION("empty ranges never overlap") {
        CHECK_FALSE(range(0, 100).overlaps(range(50, 0)));
    }

    SECTION("intersection is commutative") {
        CHECK(range(0, 100).intersection(range(50, 100)) ==
              range(50, 100).intersection(range(0, 100)));
    }
}

TEST_CASE("TimeRange::extendedBy produces the hull", "[time][timerange]") {
    CHECK(range(0, 10).extendedBy(range(20, 10)) == range(0, 30));
    CHECK(range(20, 10).extendedBy(range(0, 10)) == range(0, 30));
    CHECK(range(0, 10).extendedBy(range(2, 3)) == range(0, 10));

    SECTION("extending by an instant includes that frame") {
        CHECK(range(0, 10).extendedBy(f(20)) == range(0, 21));
        CHECK(range(10, 10).extendedBy(f(0)) == range(0, 20));
        CHECK(range(0, 10).extendedBy(f(5)) == range(0, 10));
    }
}

TEST_CASE("TimeRange::clamp lands on a frame the range owns", "[time][timerange]") {
    const TimeRange r = range(10, 5);
    CHECK(r.clamp(f(0)) == f(10));
    CHECK(r.clamp(f(12)) == f(12));
    CHECK(r.clamp(f(100)) == f(14));  // endInclusive, not endExclusive
}

TEST_CASE("TimeRange rescales both endpoints", "[time][timerange]") {
    const TimeRange r = range(25, 50);  // 1s in, 2s long, at 25fps
    const TimeRange at50 = r.rescaledTo(rates::fps50);
    CHECK(at50.start().frames() == 50);
    CHECK(at50.duration().frames() == 100);
    CHECK(at50.rescaledTo(rates::fps25) == r);
}
