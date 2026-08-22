#include <catch2/catch_test_macros.hpp>

#include "zaro/core/io/ReviewNotes.h"

using namespace zaro;

namespace {

const time::Rational k25 = time::rates::fps25;

time::TimeRange at(std::int64_t frame, std::int64_t frames = 1) {
    return time::TimeRange{time::RationalTime{frame, k25}, time::RationalTime{frames, k25}};
}

model::Sequence reviewed() {
    model::Sequence sequence{model::SequenceId{1}, "Cut 3", k25};
    std::vector<model::Marker> markers;

    model::Marker late;
    late.id = model::MarkerId{1};
    late.range = at(250);
    late.note = "the music comes in too early";
    late.author = "Priya";
    markers.push_back(late);

    model::Marker early;
    early.id = model::MarkerId{2};
    early.range = at(50);
    early.name = "Title";
    early.note = "spell the surname with two n's";
    early.author = "Sam";
    early.resolved = true;
    markers.push_back(early);

    model::Marker working;
    working.id = model::MarkerId{3};
    working.range = at(100);
    working.name = "check";  // no note, no author: somebody's own flag
    markers.push_back(working);

    sequence.setMarkers(std::move(markers));
    return sequence;
}

}  // namespace

TEST_CASE("review notes are ordered by time, not by when they were written", "[review]") {
    const std::string notes = io::reviewNotes(reviewed());
    const std::size_t sam = notes.find("Sam");
    const std::size_t priya = notes.find("Priya");
    REQUIRE(sam != std::string::npos);
    REQUIRE(priya != std::string::npos);
    // Sam's is at 2 seconds and Priya's at 10, though Priya's was added first.
    CHECK(sam < priya);
}

TEST_CASE("review notes carry timecode, author and what was said", "[review]") {
    const std::string notes = io::reviewNotes(reviewed());
    CHECK(notes.find("00:00:02:00") != std::string::npos);
    CHECK(notes.find("00:00:10:00") != std::string::npos);
    CHECK(notes.find("(Sam)") != std::string::npos);
    CHECK(notes.find("spell the surname") != std::string::npos);
    CHECK(notes.find("Title: spell") != std::string::npos);
}

TEST_CASE("resolved comments are kept and marked", "[review]") {
    const std::string notes = io::reviewNotes(reviewed());
    // Kept: what was asked for and what was done is the point of the list.
    CHECK(notes.find("spell the surname") != std::string::npos);
    CHECK(notes.find("done") != std::string::npos);
    CHECK(notes.find("1 of 2 done") != std::string::npos);
}

TEST_CASE("a working marker is not a review comment", "[review]") {
    const std::string notes = io::reviewNotes(reviewed());
    // "check" has no note and no author: it is somebody's own flag, and a
    // review list padded with those is one nobody reads twice.
    CHECK(notes.find("check") == std::string::npos);
}

TEST_CASE("a sequence with no comments says so", "[review]") {
    model::Sequence empty{model::SequenceId{2}, "Cut 1", k25};
    const std::string notes = io::reviewNotes(empty);
    CHECK(notes.find("No review comments") != std::string::npos);
    // An empty file would look like the export having failed.
    CHECK(notes.size() > 20);
}

TEST_CASE("a title can be given, and the sequence's name is the default", "[review]") {
    CHECK(io::reviewNotes(reviewed()).find("# Cut 3") != std::string::npos);
    CHECK(io::reviewNotes(reviewed(), "Notes for the grade").find("# Notes for the grade") !=
          std::string::npos);
}

TEST_CASE("a spanning comment says where it ends", "[review]") {
    model::Sequence sequence{model::SequenceId{3}, "Cut", k25};
    model::Marker span;
    span.id = model::MarkerId{1};
    span.range = at(25, 75);
    span.note = "this whole section drags";
    span.author = "Priya";
    sequence.setMarkers({span});

    const std::string notes = io::reviewNotes(sequence);
    CHECK(notes.find("00:00:01:00") != std::string::npos);
    CHECK(notes.find("00:00:04:00") != std::string::npos);
}
