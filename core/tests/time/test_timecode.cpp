#include <set>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/time/Timecode.h"

using zaro::time::framesFromTimecode;
using zaro::time::framesFromTimecodeString;
using zaro::time::framesPerTimecodeDay;
using zaro::time::isValidTimecode;
using zaro::time::parseTimecode;
using zaro::time::Rational;
using zaro::time::RationalTime;
using zaro::time::Timecode;
using zaro::time::timecodeFromFrames;
namespace rates = zaro::time::rates;

namespace {

std::string tcString(std::int64_t frame, const Rational& rate, bool drop) {
    return timecodeFromFrames(frame, rate, drop).toString();
}

/// Round trip a frame index through its label and back.
void checkRoundTrip(std::int64_t frame, const Rational& rate, bool drop) {
    const Timecode tc = timecodeFromFrames(frame, rate, drop);
    INFO("frame " << frame << " -> " << tc.toString());
    REQUIRE(isValidTimecode(tc, rate));
    const auto back = framesFromTimecode(tc, rate);
    REQUIRE(back.has_value());
    CHECK(*back == frame);
}

}  // namespace

TEST_CASE("Drop frame applies only where it is defined", "[time][timecode]") {
    CHECK(zaro::time::supportsDropFrame(rates::fps29_97));
    CHECK(zaro::time::supportsDropFrame(rates::fps59_94));
    CHECK(zaro::time::supportsDropFrame(rates::fps119_88));

    CHECK_FALSE(zaro::time::supportsDropFrame(rates::fps23_976));
    CHECK_FALSE(zaro::time::supportsDropFrame(rates::fps25));
    CHECK_FALSE(zaro::time::supportsDropFrame(rates::fps30));
    CHECK_FALSE(zaro::time::supportsDropFrame(rates::fps60));

    CHECK(zaro::time::dropFrameCount(rates::fps29_97) == 2);
    CHECK(zaro::time::dropFrameCount(rates::fps59_94) == 4);
    CHECK(zaro::time::dropFrameCount(rates::fps119_88) == 8);
    CHECK(zaro::time::dropFrameCount(rates::fps25) == 0);

    SECTION("asking for drop frame at a rate that has none is simply ignored") {
        const Timecode tc = timecodeFromFrames(0, rates::fps25, true);
        CHECK_FALSE(tc.dropFrame);
    }
}

TEST_CASE("Known drop-frame landmarks at 29.97", "[time][timecode][dropframe]") {
    const Rational& r = rates::fps29_97;

    CHECK(tcString(0, r, true) == "00:00:00;00");
    CHECK(tcString(1, r, true) == "00:00:00;01");
    CHECK(tcString(29, r, true) == "00:00:00;29");
    CHECK(tcString(30, r, true) == "00:00:01;00");
    CHECK(tcString(1798, r, true) == "00:00:59;28");
    CHECK(tcString(1799, r, true) == "00:00:59;29");

    SECTION("the first two labels of minute one do not exist") {
        CHECK(tcString(1800, r, true) == "00:01:00;02");
        CHECK(tcString(1801, r, true) == "00:01:00;03");
    }

    SECTION("every tenth minute keeps all of its labels") {
        CHECK(tcString(17982, r, true) == "00:10:00;00");
        CHECK(tcString(17982 - 1, r, true) == "00:09:59;29");
    }

    SECTION("one hour") {
        CHECK(tcString(107892, r, true) == "01:00:00;00");
    }

    SECTION("non-drop counts the same frames differently") {
        CHECK(tcString(1800, r, false) == "00:01:00:00");
        CHECK(tcString(17982, r, false) == "00:09:59:12");
        CHECK(tcString(108000, r, false) == "01:00:00:00");
    }
}

TEST_CASE("Known drop-frame landmarks at 59.94", "[time][timecode][dropframe]") {
    const Rational& r = rates::fps59_94;

    CHECK(tcString(0, r, true) == "00:00:00;00");
    CHECK(tcString(3599, r, true) == "00:00:59;59");
    CHECK(tcString(3600, r, true) == "00:01:00;04");  // four labels skipped
    CHECK(tcString(35964, r, true) == "00:10:00;00");
    CHECK(tcString(215784, r, true) == "01:00:00;00");
}

TEST_CASE("Drop frame keeps the clock honest, non-drop does not", "[time][timecode][dropframe]") {
    // The entire justification for drop frame, asserted numerically.
    const auto oneHourDrop = framesFromTimecode(Timecode{1, 0, 0, 0, true, false}, rates::fps29_97);
    const auto oneHourNonDrop =
        framesFromTimecode(Timecode{1, 0, 0, 0, false, false}, rates::fps29_97);
    REQUIRE(oneHourDrop.has_value());
    REQUIRE(oneHourNonDrop.has_value());

    const Rational elapsedDrop = RationalTime{*oneHourDrop, rates::fps29_97}.toSeconds();
    const Rational elapsedNonDrop = RationalTime{*oneHourNonDrop, rates::fps29_97}.toSeconds();
    const Rational anHour = Rational::fromInt(3600);

    CHECK((elapsedDrop - anHour).abs() < Rational::fromInt(1));
    CHECK((elapsedNonDrop - anHour).abs() > Rational::fromInt(3));
}

TEST_CASE("Labels that drop frame skips are rejected", "[time][timecode][dropframe]") {
    const Rational& r = rates::fps29_97;

    CHECK_FALSE(isValidTimecode(Timecode{0, 1, 0, 0, true, false}, r));
    CHECK_FALSE(isValidTimecode(Timecode{0, 1, 0, 1, true, false}, r));
    CHECK(isValidTimecode(Timecode{0, 1, 0, 2, true, false}, r));

    SECTION("the tenth minute keeps frames 00 and 01") {
        CHECK(isValidTimecode(Timecode{0, 10, 0, 0, true, false}, r));
        CHECK(isValidTimecode(Timecode{0, 20, 0, 1, true, false}, r));
        CHECK(isValidTimecode(Timecode{0, 0, 0, 0, true, false}, r));
    }

    SECTION("only the first second of the minute is affected") {
        CHECK(isValidTimecode(Timecode{0, 1, 1, 0, true, false}, r));
    }

    SECTION("59.94 skips four") {
        CHECK_FALSE(isValidTimecode(Timecode{0, 1, 0, 3, true, false}, rates::fps59_94));
        CHECK(isValidTimecode(Timecode{0, 1, 0, 4, true, false}, rates::fps59_94));
    }

    SECTION("out-of-range fields") {
        CHECK_FALSE(isValidTimecode(Timecode{24, 0, 0, 0, false, false}, r));
        CHECK_FALSE(isValidTimecode(Timecode{0, 60, 0, 0, false, false}, r));
        CHECK_FALSE(isValidTimecode(Timecode{0, 0, 60, 0, false, false}, r));
        CHECK_FALSE(isValidTimecode(Timecode{0, 0, 0, 30, false, false}, r));
        CHECK_FALSE(isValidTimecode(Timecode{0, 0, 0, -1, false, false}, r));
    }

    SECTION("drop frame requested at a rate that does not support it") {
        CHECK_FALSE(isValidTimecode(Timecode{0, 0, 0, 0, true, false}, rates::fps25));
    }
}

TEST_CASE("Frame count per timecode day", "[time][timecode]") {
    CHECK(framesPerTimecodeDay(rates::fps29_97, true) == 2589408);
    CHECK(framesPerTimecodeDay(rates::fps29_97, false) == 2592000);
    CHECK(framesPerTimecodeDay(rates::fps59_94, true) == 5178816);
    CHECK(framesPerTimecodeDay(rates::fps25, false) == 2160000);
    CHECK(framesPerTimecodeDay(rates::fps24, false) == 2073600);
}

TEST_CASE("Timecode wraps at 24 hours", "[time][timecode]") {
    const std::int64_t day = framesPerTimecodeDay(rates::fps29_97, true);
    CHECK(tcString(day, rates::fps29_97, true) == "00:00:00;00");
    CHECK(tcString(day + 1, rates::fps29_97, true) == "00:00:00;01");
}

TEST_CASE("Exhaustive round trip over the first hour", "[time][timecode][dropframe]") {
    // Every drop-frame boundary in the day is structurally identical to one in
    // the first hour, so an exhaustive hour plus the sampled sweep below covers
    // the space without a multi-million iteration test.
    const std::int64_t hour = 107892;
    for (std::int64_t frame = 0; frame < hour; ++frame) {
        const Timecode tc = timecodeFromFrames(frame, rates::fps29_97, true);
        const auto back = framesFromTimecode(tc, rates::fps29_97);
        if (!back || *back != frame) {
            FAIL("round trip failed at frame " << frame << " (" << tc.toString() << ")");
        }
    }
    SUCCEED("107892 frames round tripped");
}

TEST_CASE("Sampled round trip across a full 24-hour day", "[time][timecode][dropframe]") {
    struct Case {
        Rational rate;
        bool drop;
    };
    const Case cases[] = {
        {rates::fps29_97, true},  {rates::fps29_97, false}, {rates::fps59_94, true},
        {rates::fps59_94, false}, {rates::fps25, false},    {rates::fps23_976, false},
        {rates::fps119_88, true},
    };

    for (const Case& c : cases) {
        const std::int64_t day = framesPerTimecodeDay(c.rate, c.drop);

        // A stride coprime with the minute and ten-minute periods so the sweep
        // lands on a different phase of the drop pattern every time.
        for (std::int64_t frame = 0; frame < day; frame += 997) {
            checkRoundTrip(frame, c.rate, c.drop);
        }

        // Plus a window around every minute boundary, which is where the
        // skips actually happen and where an off-by-one hides.
        const std::int64_t nominal = zaro::time::nominalRate(c.rate);
        const std::int64_t drop = c.drop ? zaro::time::dropFrameCount(c.rate) : 0;
        for (std::int64_t minute = 0; minute < 1440; ++minute) {
            const std::int64_t base = nominal * 60 * minute - drop * (minute - minute / 10);
            for (std::int64_t offset = -2; offset <= 10; ++offset) {
                const std::int64_t frame = base + offset;
                if (frame >= 0 && frame < day) {
                    checkRoundTrip(frame, c.rate, c.drop);
                }
            }
        }
    }
}

TEST_CASE("Labels are unique and strictly increasing", "[time][timecode][dropframe]") {
    // Two frames must never share a label, and the labels must sort in the same
    // order as the frames. A subtly wrong drop-frame implementation usually
    // breaks one of these long before it breaks a spot check.
    std::set<std::string> seen;
    std::string previous;
    for (std::int64_t frame = 0; frame < 40000; ++frame) {
        const std::string label = tcString(frame, rates::fps29_97, true);
        if (!seen.insert(label).second) {
            FAIL("duplicate label " << label << " at frame " << frame);
        }
        if (!previous.empty() && !(previous < label)) {
            FAIL("label " << label << " does not follow " << previous);
        }
        previous = label;
    }
    SUCCEED("40000 unique, ordered labels");
}

TEST_CASE("Timecode parsing", "[time][timecode]") {
    SECTION("full form") {
        const auto tc = parseTimecode("01:23:45:12");
        REQUIRE(tc.has_value());
        CHECK(tc->hours == 1);
        CHECK(tc->minutes == 23);
        CHECK(tc->seconds == 45);
        CHECK(tc->frames == 12);
        CHECK_FALSE(tc->dropFrame);
        CHECK_FALSE(tc->negative);
    }

    SECTION("a semicolon anywhere means drop frame") {
        CHECK(parseTimecode("01:23:45;12")->dropFrame);
        CHECK(parseTimecode("01;23;45;12")->dropFrame);
    }

    SECTION("short forms fill from the right") {
        CHECK(*parseTimecode("12") == Timecode{0, 0, 0, 12, false, false});
        CHECK(*parseTimecode("5:00") == Timecode{0, 0, 5, 0, false, false});
        CHECK(*parseTimecode("2:30:00") == Timecode{0, 2, 30, 0, false, false});
    }

    SECTION("leading zeros are handled in every position") {
        const auto tc = parseTimecode("10:01:00:10");
        REQUIRE(tc.has_value());
        CHECK(tc->hours == 10);
        CHECK(tc->minutes == 1);
        CHECK(tc->seconds == 0);
        CHECK(tc->frames == 10);
    }

    SECTION("negative offsets") {
        const auto tc = parseTimecode("-00:00:01:00");
        REQUIRE(tc.has_value());
        CHECK(tc->negative);
        CHECK(tc->seconds == 1);
    }

    SECTION("rejects junk") {
        CHECK_FALSE(parseTimecode("").has_value());
        CHECK_FALSE(parseTimecode("::").has_value());
        CHECK_FALSE(parseTimecode("1:2:3:4:5").has_value());
        CHECK_FALSE(parseTimecode("aa:bb").has_value());
        CHECK_FALSE(parseTimecode("01:00:").has_value());
    }
}

TEST_CASE("Timecode string round trip", "[time][timecode]") {
    for (const std::int64_t frame : {0LL, 1LL, 1800LL, 17982LL, 107892LL, 1234567LL}) {
        const Timecode tc = timecodeFromFrames(frame, rates::fps29_97, true);
        const auto reparsed = parseTimecode(tc.toString());
        REQUIRE(reparsed.has_value());
        CHECK(*reparsed == tc);
        CHECK(framesFromTimecodeString(tc.toString(), rates::fps29_97) == frame);
    }
}

TEST_CASE("Negative timecodes round trip", "[time][timecode]") {
    const Timecode tc = timecodeFromFrames(-1800, rates::fps29_97, false);
    CHECK(tc.negative);
    CHECK(tc.toString() == "-00:01:00:00");
    CHECK(framesFromTimecode(tc, rates::fps29_97) == -1800);
}
