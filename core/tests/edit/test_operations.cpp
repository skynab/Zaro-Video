#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"

#include "ModelFixtures.h"

using namespace zaro;
using edit::Edge;
using zaro::testing::Fixture;

TEST_CASE("Overwrite drops a clip on top of what is there", "[edit][overwrite]") {
    Fixture f;

    SECTION("onto empty track") {
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
        CHECK(f.layout(f.v1) == "0-50@500");
    }

    SECTION("fully covering an existing clip removes it") {
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(10, 20))));
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 700))));
        CHECK(f.layout(f.v1) == "0-50@700");
    }

    SECTION("partly covering trims the neighbour, taking its source with it") {
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(30, 40, 900))));
        // The first clip keeps frames 0-30 and therefore source 500-530.
        CHECK(f.layout(f.v1) == "0-30@500 30-70@900");
    }

    SECTION("landing inside a clip splits it in two") {
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 100, 500))));
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(40, 20, 900))));
        // The tail resumes at source 560, which is where it left off.
        CHECK(f.layout(f.v1) == "0-40@500 40-60@900 60-100@560");
    }

    SECTION("the split halves get distinct ids") {
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 100))));
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(40, 20))));
        const auto& clips = f.track(f.v1).clips();
        REQUIRE(clips.size() == 3);
        CHECK(clips[0].id != clips[2].id);
    }
}

TEST_CASE("Insert pushes what follows to the right", "[edit][insert]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 600))));

    SECTION("at a cut, nothing is split") {
        REQUIRE(f.run(edit::makeInsert(f.project, f.on(f.v1), f.clip(50, 25, 900))));
        CHECK(f.layout(f.v1) == "0-50@500 50-75@900 75-125@600");
    }

    SECTION("mid clip, the clip is split around the insertion") {
        REQUIRE(f.run(edit::makeInsert(f.project, f.on(f.v1), f.clip(20, 30, 900))));
        CHECK(f.layout(f.v1) == "0-20@500 20-50@900 50-80@520 80-130@600");
    }

    SECTION("rippling all tracks keeps other tracks in sync") {
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 300))));
        REQUIRE(f.run(edit::makeInsert(f.project, f.on(f.v1), f.clip(50, 25, 900), true)));
        // The audio clip is split at 50 and its tail moves with the picture.
        CHECK(f.layout(f.a1) == "0-50@300 75-125@350");
    }
}

TEST_CASE("Razor splits a clip at a point", "[edit][razor]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 100, 500))));

    SECTION("mid clip") {
        REQUIRE(f.run(edit::makeRazor(f.project, f.on(f.v1), f.at(30))));
        CHECK(f.layout(f.v1) == "0-30@500 30-100@530");
    }

    SECTION("on an existing cut is refused rather than making a zero-length clip") {
        REQUIRE_FALSE(f.run(edit::makeRazor(f.project, f.on(f.v1), f.at(0))));
        CHECK(f.lastError.find("already a cut") != std::string::npos);
    }

    SECTION("in a gap is refused") {
        REQUIRE_FALSE(f.run(edit::makeRazor(f.project, f.on(f.v1), f.at(500))));
    }
}

TEST_CASE("Lift leaves a gap, extract closes it", "[edit][lift][extract]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 600))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(100, 50, 700))));
    const model::ClipId middle = f.track(f.v1).clips()[1].id;

    SECTION("lift") {
        REQUIRE(f.run(edit::makeLift(f.project, f.on(f.v1), middle)));
        CHECK(f.layout(f.v1) == "0-50@500 100-150@700");
    }

    SECTION("extract") {
        REQUIRE(f.run(edit::makeExtract(f.project, f.on(f.v1), middle)));
        CHECK(f.layout(f.v1) == "0-50@500 50-100@700");
    }
}

TEST_CASE("Ripple delete removes a range and closes the gap", "[edit][ripple]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 600))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(100, 50, 700))));

    SECTION("spanning a whole clip") {
        REQUIRE(f.run(edit::makeRippleDelete(f.project, f.on(f.v1), f.range(50, 50))));
        CHECK(f.layout(f.v1) == "0-50@500 50-100@700");
    }

    SECTION("cutting across two clips trims both") {
        REQUIRE(f.run(edit::makeRippleDelete(f.project, f.on(f.v1), f.range(30, 40))));
        CHECK(f.layout(f.v1) == "0-30@500 30-60@620 60-110@700");
    }
}

TEST_CASE("Trim moves one edge and leaves neighbours alone", "[edit][trim]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    const model::ClipId id = f.track(f.v1).clips()[0].id;

    SECTION("in point later shortens from the front, and takes the source with it") {
        REQUIRE(f.run(edit::makeTrim(f.project, f.on(f.v1), id, Edge::In, f.at(10))));
        CHECK(f.layout(f.v1) == "10-50@510");
    }

    SECTION("in point earlier lengthens from the front") {
        // From a clip that does not start at zero, so the trim is not refused
        // for running off the front of the sequence.
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v2), f.clip(100, 50, 500))));
        const model::ClipId later = f.track(f.v2).clips()[0].id;
        REQUIRE(f.run(edit::makeTrim(f.project, f.on(f.v2), later, Edge::In, f.at(-10))));
        CHECK(f.layout(f.v2) == "90-150@490");
    }

    SECTION("a trim that would start the clip before the sequence is refused") {
        REQUIRE_FALSE(f.run(edit::makeTrim(f.project, f.on(f.v1), id, Edge::In, f.at(-10))));
        CHECK(f.lastError.find("before the sequence") != std::string::npos);
    }

    SECTION("out point later lengthens the tail") {
        REQUIRE(f.run(edit::makeTrim(f.project, f.on(f.v1), id, Edge::Out, f.at(10))));
        CHECK(f.layout(f.v1) == "0-60@500");
    }

    SECTION("trimming away the whole clip is refused") {
        REQUIRE_FALSE(f.run(edit::makeTrim(f.project, f.on(f.v1), id, Edge::In, f.at(50))));
        CHECK(f.lastError.find("nothing of the clip") != std::string::npos);
    }

    SECTION("trimming into a neighbour is refused") {
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(60, 50, 600))));
        REQUIRE_FALSE(f.run(edit::makeTrim(f.project, f.on(f.v1), id, Edge::Out, f.at(20))));
        CHECK(f.lastError.find("neighbouring clip") != std::string::npos);
    }
}

TEST_CASE("Trim respects the extent of the source media", "[edit][trim][bounds]") {
    Fixture f;
    // A clip using the whole of the short media: there is nothing left to
    // extend into at either end.
    // Parked well down the timeline so that running out of *source* is the
    // constraint under test, not running off the front of the sequence.
    model::Clip clip = f.clip(200, Fixture::kShortMediaFrames, 0, f.shortMedia);
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), clip)));
    const model::ClipId id = f.track(f.v1).clips()[0].id;

    SECTION("extending past the end is refused") {
        REQUIRE_FALSE(f.run(edit::makeTrim(f.project, f.on(f.v1), id, Edge::Out, f.at(1))));
        CHECK(f.lastError.find("past the end of the source") != std::string::npos);
    }

    SECTION("extending before the start is refused") {
        REQUIRE_FALSE(f.run(edit::makeTrim(f.project, f.on(f.v1), id, Edge::In, f.at(-1))));
        CHECK(f.lastError.find("off the start of the source") != std::string::npos);
    }

    SECTION("trimming inward is still fine") {
        REQUIRE(f.run(edit::makeTrim(f.project, f.on(f.v1), id, Edge::Out, f.at(-10))));
        CHECK(f.layout(f.v1) == "200-290@0");
    }
}

TEST_CASE("Ripple trim closes the gap it would have opened", "[edit][trim][ripple]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 600))));
    const model::ClipId first = f.track(f.v1).clips()[0].id;

    SECTION("shortening the out point pulls the rest back") {
        REQUIRE(f.run(edit::makeRippleTrim(f.project, f.on(f.v1), first, Edge::Out, f.at(-10))));
        CHECK(f.layout(f.v1) == "0-40@500 40-90@600");
    }

    SECTION("lengthening the out point pushes the rest along") {
        REQUIRE(f.run(edit::makeRippleTrim(f.project, f.on(f.v1), first, Edge::Out, f.at(10))));
        CHECK(f.layout(f.v1) == "0-60@500 60-110@600");
    }

    SECTION("trimming the in point does not move the clip, it shortens it in place") {
        REQUIRE(f.run(edit::makeRippleTrim(f.project, f.on(f.v1), first, Edge::In, f.at(10))));
        CHECK(f.layout(f.v1) == "0-40@510 40-90@600");
    }
}

TEST_CASE("Roll moves a cut without changing total duration", "[edit][roll]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 600))));
    const model::ClipId left = f.track(f.v1).clips()[0].id;
    const time::RationalTime before = f.track(f.v1).extent().duration();

    SECTION("later") {
        REQUIRE(f.run(edit::makeRoll(f.project, f.on(f.v1), left, f.at(10))));
        CHECK(f.layout(f.v1) == "0-60@500 60-100@610");
        CHECK(f.track(f.v1).extent().duration() == before);
    }

    SECTION("earlier") {
        REQUIRE(f.run(edit::makeRoll(f.project, f.on(f.v1), left, f.at(-10))));
        CHECK(f.layout(f.v1) == "0-40@500 40-100@590");
        CHECK(f.track(f.v1).extent().duration() == before);
    }

    SECTION("across a gap is refused") {
        Fixture g;
        REQUIRE(g.run(edit::makeOverwrite(g.project, g.on(g.v1), g.clip(0, 50))));
        REQUIRE(g.run(edit::makeOverwrite(g.project, g.on(g.v1), g.clip(60, 50))));
        const model::ClipId id = g.track(g.v1).clips()[0].id;
        REQUIRE_FALSE(g.run(edit::makeRoll(g.project, g.on(g.v1), id, g.at(5))));
        CHECK(g.lastError.find("gap") != std::string::npos);
    }

    SECTION("consuming a whole clip is refused") {
        REQUIRE_FALSE(f.run(edit::makeRoll(f.project, f.on(f.v1), left, f.at(50))));
    }
}

TEST_CASE("Slip changes content without moving anything", "[edit][slip]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 600))));
    const model::ClipId first = f.track(f.v1).clips()[0].id;

    REQUIRE(f.run(edit::makeSlip(f.project, f.on(f.v1), first, f.at(25))));
    CHECK(f.layout(f.v1) == "0-50@525 50-100@600");

    SECTION("and is bounded by the available source") {
        model::Clip clip = f.clip(200, Fixture::kShortMediaFrames, 0, f.shortMedia);
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v2), clip)));
        const model::ClipId id = f.track(f.v2).clips()[0].id;
        REQUIRE_FALSE(f.run(edit::makeSlip(f.project, f.on(f.v2), id, f.at(1))));
        CHECK(f.lastError.find("slip ran out of source") != std::string::npos);
    }
}

TEST_CASE("Slide moves a clip and its neighbours absorb it", "[edit][slide]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 600))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(100, 50, 700))));
    const model::ClipId middle = f.track(f.v1).clips()[1].id;
    const time::RationalTime before = f.track(f.v1).extent().duration();

    REQUIRE(f.run(edit::makeSlide(f.project, f.on(f.v1), middle, f.at(10))));
    // The middle clip keeps its content and duration; the outer two give and take.
    CHECK(f.layout(f.v1) == "0-60@500 60-110@600 110-150@710");
    CHECK(f.track(f.v1).extent().duration() == before);

    SECTION("an end clip has nothing to slide against") {
        const model::ClipId first = f.track(f.v1).clips()[0].id;
        REQUIRE_FALSE(f.run(edit::makeSlide(f.project, f.on(f.v1), first, f.at(5))));
        CHECK(f.lastError.find("clip on each side") != std::string::npos);
    }
}

TEST_CASE("Move relocates a clip, overwriting the destination", "[edit][move]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    const model::ClipId id = f.track(f.v1).clips()[0].id;

    SECTION("within a track") {
        REQUIRE(f.run(edit::makeMove(f.project, f.on(f.v1), id, f.v1, f.at(200))));
        CHECK(f.layout(f.v1) == "200-250@500");
    }

    SECTION("to another track") {
        REQUIRE(f.run(edit::makeMove(f.project, f.on(f.v1), id, f.v2, f.at(100))));
        CHECK(f.layout(f.v1).empty());
        CHECK(f.layout(f.v2) == "100-150@500");
    }

    SECTION("before the start of the sequence is refused") {
        REQUIRE_FALSE(f.run(edit::makeMove(f.project, f.on(f.v1), id, f.v1, f.at(-10))));
    }
}

TEST_CASE("Locked tracks refuse edits", "[edit][locks]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    f.track(f.v1).setLocked(true);

    REQUIRE_FALSE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(100, 50))));
    CHECK(f.lastError.find("locked") != std::string::npos);
    CHECK(f.track(f.v1).clips().size() == 1);
}

TEST_CASE("Operations validate before touching the model", "[edit]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const std::string before = f.layout(f.v1);

    // A failed build must leave no trace, which is the whole reason validation
    // happens at construction rather than inside apply().
    CHECK_FALSE(f.run(edit::makeRazor(f.project, f.on(f.v1), f.at(9999))));
    CHECK_FALSE(f.run(edit::makeLift(f.project, f.on(f.v1), model::ClipId{9999})));
    CHECK_FALSE(
        f.run(edit::makeOverwrite(f.project, {f.sequenceId, model::TrackId{9999}}, f.clip(0, 10))));
    CHECK(f.layout(f.v1) == before);
    CHECK(f.stack.depth() == 1);
}

TEST_CASE("Add and remove tracks", "[edit][tracks]") {
    Fixture f;
    const std::size_t before = f.sequence().videoTracks().size();
    REQUIRE(f.run(edit::makeAddTrack(f.project, f.sequenceId, model::TrackKind::Video, "V3")));
    CHECK(f.sequence().videoTracks().size() == before + 1);
    CHECK(f.sequence().videoTracks().back().name() == "V3");

    REQUIRE(f.run(edit::makeRemoveTrack(f.project, f.sequenceId, f.v2)));
    CHECK(f.sequence().findTrack(f.v2) == nullptr);
}
