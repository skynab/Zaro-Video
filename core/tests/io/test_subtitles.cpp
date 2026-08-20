#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/io/SubtitleIo.h"

using namespace zaro;
using Catch::Approx;

namespace {

std::int64_t startMs(const model::Caption& caption) {
    return caption.range.start().rescaledTo(time::Rational{1000, 1}).frames();
}
std::int64_t endMs(const model::Caption& caption) {
    return caption.range.endExclusive().rescaledTo(time::Rational{1000, 1}).frames();
}

}  // namespace

TEST_CASE("A SubRip file reads back what it says", "[io][captions]") {
    const std::string text =
        "1\n"
        "00:00:01,000 --> 00:00:04,500\n"
        "First line\n"
        "Second line\n"
        "\n"
        "2\n"
        "00:01:02,250 --> 00:01:03,000\n"
        "Later\n";

    const auto track = io::parseSubtitles(text);
    REQUIRE(track);
    REQUIRE(track->size() == 2);

    CHECK(startMs(track->captions()[0]) == 1000);
    CHECK(endMs(track->captions()[0]) == 4500);
    // Newlines are kept as they are: every subtitle format stores lines this
    // way, and splitting on the way in only to rejoin on the way out invents
    // differences.
    CHECK(track->captions()[0].text == "First line\nSecond line");
    CHECK(startMs(track->captions()[1]) == 62250);
}

TEST_CASE("WebVTT is the same reader", "[io][captions]") {
    // A .vtt is a .srt with dots and a header, plus features nobody uses in an
    // edit. Refusing one would be pedantry rather than correctness.
    const std::string text =
        "WEBVTT\n"
        "\n"
        "00:01.000 --> 00:04.000 line:90% align:middle\n"
        "No hours, and cue settings after the timestamps\n";

    const auto track = io::parseSubtitles(text);
    REQUIRE(track);
    REQUIRE(track->size() == 1);
    CHECK(startMs(track->captions()[0]) == 1000);
    CHECK(endMs(track->captions()[0]) == 4000);
    CHECK(track->captions()[0].text == "No hours, and cue settings after the timestamps");
}

TEST_CASE("Fractional seconds are read as milliseconds however they are written",
          "[io][captions]") {
    // Files in the wild write one, two or three digits, and "5" is not five
    // milliseconds.
    const auto track = io::parseSubtitles(
        "00:00:00,5 --> 00:00:01,05\n"
        "padded\n");
    REQUIRE(track);
    CHECK(startMs(track->captions()[0]) == 500);
    CHECK(endMs(track->captions()[0]) == 1050);
}

TEST_CASE("A byte order mark does not become part of the first cue", "[io][captions]") {
    const std::string text =
        "\xEF\xBB\xBF"
        "1\n"
        "00:00:01,000 --> 00:00:02,000\n"
        "Hello\n";
    const auto track = io::parseSubtitles(text);
    REQUIRE(track);
    REQUIRE(track->size() == 1);
    CHECK(track->captions()[0].text == "Hello");
}

TEST_CASE("Windows line endings are handled", "[io][captions]") {
    const auto track = io::parseSubtitles("1\r\n00:00:01,000 --> 00:00:02,000\r\nHello\r\n\r\n");
    REQUIRE(track);
    REQUIRE(track->size() == 1);
    CHECK(track->captions()[0].text == "Hello");
}

TEST_CASE("Captions are ordered by start time however the file was written", "[io][captions]") {
    const auto track = io::parseSubtitles(
        "1\n00:00:09,000 --> 00:00:10,000\nLater\n\n"
        "2\n00:00:01,000 --> 00:00:02,000\nEarlier\n");
    REQUIRE(track);
    REQUIRE(track->size() == 2);
    CHECK(track->captions()[0].text == "Earlier");
    CHECK(track->captions()[1].text == "Later");
}

TEST_CASE("Overlapping captions are both kept", "[io][captions]") {
    // Formats allow them, and a reader that assumed one at a time would drop
    // the second half of every conversation where two people speak over each
    // other.
    const auto track = io::parseSubtitles(
        "1\n00:00:01,000 --> 00:00:05,000\nOne speaker\n\n"
        "2\n00:00:02,000 --> 00:00:06,000\nAnother\n");
    REQUIRE(track);
    REQUIRE(track->size() == 2);

    const auto showing = track->at(time::RationalTime{3, time::Rational{1, 1}});
    CHECK(showing.size() == 2);
    const auto alone = track->at(time::RationalTime{5500, time::Rational{1000, 1}});
    CHECK(alone.size() == 1);
    const auto none = track->at(time::RationalTime{20, time::Rational{1, 1}});
    CHECK(none.empty());
}

TEST_CASE("A malformed file is refused rather than half read", "[io][captions]") {
    CHECK_FALSE(io::parseSubtitles("not a subtitle file at all\n"));
    CHECK_FALSE(io::parseSubtitles("1\n00:00:0a,000 --> 00:00:02,000\nHello\n"));
    // Ending before it starts is the kind of file that produces captions which
    // never appear, which is worse than not loading.
    CHECK_FALSE(io::parseSubtitles("1\n00:00:05,000 --> 00:00:02,000\nBackwards\n"));
    CHECK_FALSE(io::parseSubtitles(""));
    CHECK_FALSE(io::loadSubtitles("/definitely/not/a/file.srt"));
}

TEST_CASE("Writing and reading are inverses", "[io][captions]") {
    // Milliseconds throughout, so a round trip is exact rather than nearly so.
    const std::string original =
        "1\n00:00:01,000 --> 00:00:04,500\nFirst\nSecond\n\n"
        "2\n01:02:03,004 --> 01:02:04,000\nLater\n";
    const auto track = io::parseSubtitles(original);
    REQUIRE(track);

    for (const io::SubtitleFormat format :
         {io::SubtitleFormat::SubRip, io::SubtitleFormat::WebVtt}) {
        const std::string written = io::writeSubtitles(*track, format);
        const auto again = io::parseSubtitles(written);
        REQUIRE(again);
        REQUIRE(again->size() == track->size());
        for (std::size_t i = 0; i < track->size(); ++i) {
            CHECK(startMs(again->captions()[i]) == startMs(track->captions()[i]));
            CHECK(endMs(again->captions()[i]) == endMs(track->captions()[i]));
            CHECK(again->captions()[i].text == track->captions()[i].text);
        }
    }
}

TEST_CASE("SubRip is written with commas and WebVTT with dots", "[io][captions]") {
    const auto track = io::parseSubtitles("1\n00:00:01,500 --> 00:00:02,000\nHi\n");
    REQUIRE(track);

    const std::string srt = io::writeSubtitles(*track, io::SubtitleFormat::SubRip);
    CHECK(srt.find("00:00:01,500 --> 00:00:02,000") != std::string::npos);
    CHECK(srt.rfind("1\n", 0) == 0);  // SubRip numbers its cues

    const std::string vtt = io::writeSubtitles(*track, io::SubtitleFormat::WebVtt);
    CHECK(vtt.rfind("WEBVTT", 0) == 0);
    CHECK(vtt.find("00:00:01.500 --> 00:00:02.000") != std::string::npos);
}

TEST_CASE("The extension picks the format", "[io][captions]") {
    CHECK(io::formatForPath("/a/b/subs.vtt") == io::SubtitleFormat::WebVtt);
    CHECK(io::formatForPath("/a/b/subs.VTT") == io::SubtitleFormat::WebVtt);
    CHECK(io::formatForPath("/a/b/subs.srt") == io::SubtitleFormat::SubRip);
    CHECK(io::formatForPath("subs") == io::SubtitleFormat::SubRip);
}
