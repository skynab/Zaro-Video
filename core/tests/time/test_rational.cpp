#include <catch2/catch_test_macros.hpp>

#include "zaro/core/time/Rational.h"

using zaro::time::nominalRate;
using zaro::time::Rational;
namespace rates = zaro::time::rates;

TEST_CASE("Rational normalises on construction", "[time][rational]") {
    SECTION("reduces to lowest terms") {
        const Rational r{6, 8};
        CHECK(r.num() == 3);
        CHECK(r.den() == 4);
    }

    SECTION("moves the sign to the numerator") {
        const Rational r{1, -2};
        CHECK(r.num() == -1);
        CHECK(r.den() == 2);
    }

    SECTION("normalises zero to 0/1 regardless of denominator") {
        CHECK(Rational{0, 99} == Rational{0, 1});
    }

    SECTION("broadcast rates keep their exact 1001 denominators") {
        CHECK(rates::fps29_97.num() == 30000);
        CHECK(rates::fps29_97.den() == 1001);
        CHECK(rates::fps23_976.num() == 24000);
        CHECK(rates::fps23_976.den() == 1001);
    }
}

TEST_CASE("Rational arithmetic stays exact", "[time][rational]") {
    SECTION("thirds sum back to a whole without drift") {
        Rational sum{0, 1};
        for (int i = 0; i < 3; ++i) {
            sum += Rational{1, 3};
        }
        CHECK(sum == Rational::fromInt(1));
    }

    SECTION("a thousand 1001ths of a frame stay exact") {
        // The float version of this loop is off by ~1e-13 -- small, but it is
        // the same error that becomes a dropped frame an hour into a timeline.
        Rational sum{0, 1};
        for (int i = 0; i < 30000; ++i) {
            sum += rates::fps29_97.inverse();
        }
        CHECK(sum == Rational{1001, 1});
    }

    SECTION("subtraction and negation") {
        CHECK(Rational{1, 2} - Rational{1, 3} == Rational{1, 6});
        CHECK(-Rational{1, 2} == Rational{-1, 2});
        CHECK((Rational{-3, 4}).abs() == Rational{3, 4});
    }

    SECTION("multiplication and division") {
        CHECK(Rational{2, 3} * Rational{3, 2} == Rational::fromInt(1));
        CHECK(Rational{1, 2} / Rational{1, 4} == Rational::fromInt(2));
        CHECK(rates::fps29_97.inverse() == Rational{1001, 30000});
    }

    SECTION("large magnitudes do not overflow because cross terms reduce first") {
        // 24 hours of 48kHz samples over a 1001-denominator rate.
        const Rational seconds{86400, 1};
        const Rational samples = seconds * rates::hz48000;
        CHECK(samples == Rational::fromInt(4147200000LL));

        const Rational frames = seconds * rates::fps59_94;
        CHECK(frames.den() == 1001);
        CHECK(frames.num() == 86400LL * 60000LL);
    }
}

TEST_CASE("Rational ordering compares values, not representations", "[time][rational]") {
    CHECK(Rational{1, 2} == Rational{50, 100});
    CHECK(Rational{1, 3} < Rational{1, 2});
    CHECK(Rational{-1, 3} < Rational{1, 1000000});
    CHECK(rates::fps23_976 < rates::fps24);
    CHECK(rates::fps29_97 < rates::fps30);
    CHECK(rates::fps60 > rates::fps59_94);
}

TEST_CASE("Rational rounding", "[time][rational]") {
    SECTION("floor rounds toward negative infinity") {
        CHECK(Rational{7, 2}.floorToInt() == 3);
        CHECK(Rational{-7, 2}.floorToInt() == -4);
        CHECK(Rational{4, 2}.floorToInt() == 2);
    }

    SECTION("ceil rounds toward positive infinity") {
        CHECK(Rational{7, 2}.ceilToInt() == 4);
        CHECK(Rational{-7, 2}.ceilToInt() == -3);
        CHECK(Rational{4, 2}.ceilToInt() == 2);
    }

    SECTION("round breaks ties away from zero") {
        CHECK(Rational{1, 2}.roundToInt() == 1);
        CHECK(Rational{-1, 2}.roundToInt() == -1);
        CHECK(Rational{3, 2}.roundToInt() == 2);
        CHECK(Rational{-3, 2}.roundToInt() == -2);
        CHECK(Rational{14, 10}.roundToInt() == 1);
        CHECK(Rational{-14, 10}.roundToInt() == -1);
        CHECK(Rational{16, 10}.roundToInt() == 2);
    }
}

TEST_CASE("nominalRate is the number timecode counts in", "[time][rational]") {
    CHECK(nominalRate(rates::fps23_976) == 24);
    CHECK(nominalRate(rates::fps29_97) == 30);
    CHECK(nominalRate(rates::fps59_94) == 60);
    CHECK(nominalRate(rates::fps119_88) == 120);
    CHECK(nominalRate(rates::fps25) == 25);
    CHECK(nominalRate(rates::fps30) == 30);
}

TEST_CASE("Rational parsing", "[time][rational]") {
    SECTION("fractions") {
        CHECK(Rational::parse("30000/1001") == rates::fps29_97);
        CHECK(Rational::parse("24/1") == rates::fps24);
        CHECK(Rational::parse("-1/2") == Rational{-1, 2});
    }

    SECTION("integers") {
        CHECK(Rational::parse("25") == rates::fps25);
        CHECK(Rational::parse(" 60 ") == rates::fps60);
    }

    SECTION("decimals snap to the broadcast rate they were rounded from") {
        CHECK(Rational::parse("23.976") == rates::fps23_976);
        CHECK(Rational::parse("29.97") == rates::fps29_97);
        CHECK(Rational::parse("59.94") == rates::fps59_94);
        CHECK(Rational::parse("119.88") == rates::fps119_88);
    }

    SECTION("decimals far from a known rate stay exact") {
        CHECK(Rational::parse("1.5") == Rational{3, 2});
        CHECK(Rational::parse("0.125") == Rational{1, 8});
    }

    SECTION("rejects junk") {
        CHECK_FALSE(Rational::parse("").has_value());
        CHECK_FALSE(Rational::parse("abc").has_value());
        CHECK_FALSE(Rational::parse("1/0").has_value());
        CHECK_FALSE(Rational::parse("1/").has_value());
        CHECK_FALSE(Rational::parse("1.2.3").has_value());
    }
}

TEST_CASE("Rational::approximate recovers exact rates from doubles", "[time][rational]") {
    CHECK(Rational::approximate(30000.0 / 1001.0) == rates::fps29_97);
    CHECK(Rational::approximate(24000.0 / 1001.0) == rates::fps23_976);
    CHECK(Rational::approximate(25.0) == rates::fps25);
    CHECK(Rational::approximate(0.5) == Rational{1, 2});
    CHECK(Rational::approximate(-0.25) == Rational{-1, 4});
}

TEST_CASE("Rational string round trip", "[time][rational]") {
    CHECK(rates::fps29_97.toString() == "30000/1001");
    CHECK(rates::fps25.toString() == "25");
    CHECK(Rational::parse(rates::fps59_94.toString()) == rates::fps59_94);
}
