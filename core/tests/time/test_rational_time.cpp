#include <catch2/catch_test_macros.hpp>

#include "zaro/core/time/RationalTime.h"

using zaro::time::Rational;
using zaro::time::RationalTime;
namespace rates = zaro::time::rates;

TEST_CASE("RationalTime converts to exact seconds", "[time][rationaltime]") {
    SECTION("integer rates are trivially exact") {
        CHECK(RationalTime{48, rates::fps24}.toSeconds() == Rational::fromInt(2));
        CHECK(RationalTime{25, rates::fps25}.toSeconds() == Rational::fromInt(1));
    }

    SECTION("1001 rates keep their denominator instead of rounding") {
        const RationalTime oneHourOfFrames{107892, rates::fps29_97};
        CHECK(oneHourOfFrames.toSeconds() == Rational{107892LL * 1001LL, 30000LL});
        // 3599.3928s -- an hour of 29.97 timecode is NOT an hour of wall clock.
        CHECK(oneHourOfFrames.toSeconds() < Rational::fromInt(3600));
    }

    SECTION("zero and negative") {
        CHECK(RationalTime{0, rates::fps30}.toSeconds().isZero());
        CHECK(RationalTime{-30, rates::fps30}.toSeconds() == Rational::fromInt(-1));
    }
}

TEST_CASE("RationalTime::fromSeconds rounds to the nearest frame", "[time][rationaltime]") {
    CHECK(RationalTime::fromSeconds(Rational::fromInt(2), rates::fps24).frames() == 48);
    CHECK(RationalTime::fromSeconds(Rational{1, 2}, rates::fps25).frames() == 13);
    CHECK(RationalTime::fromSeconds(Rational{-1, 2}, rates::fps25).frames() == -13);
    CHECK(RationalTime::fromSeconds(Rational::fromInt(1), rates::fps29_97).frames() == 30);
}

TEST_CASE("RationalTime rescaling between rates", "[time][rationaltime]") {
    SECTION("23.976 to 25 is exact at whole-second boundaries") {
        // 24000 frames at 24000/1001 is exactly 1001 seconds.
        const RationalTime source{24000, rates::fps23_976};
        REQUIRE(source.toSeconds() == Rational::fromInt(1001));

        const RationalTime target = source.rescaledTo(rates::fps25);
        CHECK(target.frames() == 1001 * 25);
        CHECK(target.rate() == rates::fps25);
        CHECK(target.toSeconds() == Rational::fromInt(1001));
    }

    SECTION("25 back to 23.976 recovers the original frame") {
        const RationalTime source{24000, rates::fps23_976};
        CHECK(source.rescaledTo(rates::fps25).rescaledTo(rates::fps23_976) == source);
    }

    SECTION("integer multiples are lossless in both directions") {
        const RationalTime at24{100, rates::fps24};
        CHECK(at24.rescaledTo(rates::fps48).frames() == 200);
        CHECK(at24.rescaledTo(rates::fps48).rescaledTo(rates::fps24) == at24);
        CHECK(RationalTime{1, rates::fps29_97}.rescaledTo(rates::fps59_94).frames() == 2);
    }

    SECTION("rescaling to the same rate is identity") {
        const RationalTime t{12345, rates::fps59_94};
        CHECK(t.rescaledTo(rates::fps59_94) == t);
    }

    SECTION("a non-multiple rescale rounds, and says so by not round-tripping") {
        const RationalTime at25{1, rates::fps25};  // 0.04s
        const RationalTime at24 = at25.rescaledTo(rates::fps24);
        CHECK(at24.frames() == 1);  // 0.041666s, the nearest 24fps frame
        CHECK(at24 != at25);
    }
}

TEST_CASE("RationalTime compares by instant, not representation", "[time][rationaltime]") {
    CHECK(RationalTime{12, rates::fps24} == RationalTime{24, rates::fps48});
    CHECK(RationalTime{1, rates::fps24} > RationalTime{1, rates::fps48});
    CHECK(RationalTime{0, rates::fps24} == RationalTime{0, rates::fps29_97});
    CHECK(RationalTime{30, rates::fps29_97} > RationalTime{30, rates::fps30});
    CHECK(RationalTime{-1, rates::fps24} < RationalTime{0, rates::fps25});
}

TEST_CASE("RationalTime arithmetic", "[time][rationaltime]") {
    SECTION("same rate adds frame counts directly") {
        const RationalTime sum = RationalTime{10, rates::fps25} + RationalTime{15, rates::fps25};
        CHECK(sum.frames() == 25);
        CHECK(sum.rate() == rates::fps25);
    }

    SECTION("mixed rates resolve to the finer rate") {
        const RationalTime sum = RationalTime{1, rates::fps24} + RationalTime{1, rates::fps48};
        CHECK(sum.rate() == rates::fps48);
        CHECK(sum.frames() == 3);
    }

    SECTION("subtraction of equal instants is zero at either rate") {
        CHECK((RationalTime{12, rates::fps24} - RationalTime{24, rates::fps48}).isZero());
    }

    SECTION("accumulating a frame at a time does not drift") {
        RationalTime t{0, rates::fps29_97};
        const RationalTime oneFrame{1, rates::fps29_97};
        for (int i = 0; i < 108000; ++i) {
            t += oneFrame;
        }
        CHECK(t.frames() == 108000);
        CHECK(t.toSeconds() == Rational{108000LL * 1001LL, 30000LL});
    }

    SECTION("negation and abs") {
        CHECK((-RationalTime{5, rates::fps24}).frames() == -5);
        CHECK(RationalTime{-5, rates::fps24}.abs().frames() == 5);
    }
}

TEST_CASE("RationalTime string form is diagnostic, not display", "[time][rationaltime]") {
    CHECK(RationalTime{120, rates::fps29_97}.toString() == "120@30000/1001");
}
