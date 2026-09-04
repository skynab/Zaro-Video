#include <cstdint>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"

#include "ModelFixtures.h"

using namespace zaro;
using Catch::Approx;
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

TEST_CASE("Pasting puts a set of clips down as one edit", "[edit][paste]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 600))));

    // Two clips copied and put down at 200, keeping the gap between them.
    // Fresh ids, because the originals are still on the timeline.
    std::vector<edit::PastedClip> placing;
    for (const model::Clip& original : f.track(f.v1).clips()) {
        model::Clip copy = original;
        copy.id = f.project.ids().next<model::ClipTag>();
        copy.timelineRange = time::TimeRange{original.start() + f.at(200), original.duration()};
        placing.push_back(edit::PastedClip{f.v1, copy});
    }

    REQUIRE(f.run(edit::makePasteClips(f.project, f.sequenceId, placing)));
    CHECK(f.layout(f.v1) == "0-50@500 50-100@600 200-250@500 250-300@600");

    SECTION("and one undo takes the whole paste back") {
        // The point of it being one command: four clips went down, and nobody
        // expects to press Ctrl+Z twice for one Ctrl+V.
        f.stack.undo(f.project);
        CHECK(f.layout(f.v1) == "0-50@500 50-100@600");
    }
}

TEST_CASE("A paste that cannot land is refused before anything moves", "[edit][paste]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    const std::string before = f.layout(f.v1);

    const auto pasted = [&](std::int64_t at, model::TrackId track, bool freshId) {
        model::Clip copy = f.track(f.v1).clips().front();
        if (freshId) {
            copy.id = f.project.ids().next<model::ClipTag>();
        }
        copy.timelineRange = time::TimeRange{f.at(at), copy.duration()};
        return std::vector<edit::PastedClip>{edit::PastedClip{track, copy}};
    };

    SECTION("nothing to paste") {
        CHECK_FALSE(edit::makePasteClips(f.project, f.sequenceId, {}));
    }
    SECTION("a track that is not in this sequence") {
        CHECK_FALSE(
            edit::makePasteClips(f.project, f.sequenceId, pasted(100, model::TrackId{}, true)));
    }
    SECTION("an id already on the timeline") {
        // Reusing the copied clip's own id would put two clips with one
        // identity on the same track, and every later edit would pick whichever
        // it found first.
        CHECK_FALSE(edit::makePasteClips(f.project, f.sequenceId, pasted(100, f.v1, false)));
    }
    SECTION("before the start of the timeline") {
        CHECK_FALSE(edit::makePasteClips(f.project, f.sequenceId, pasted(-10, f.v1, true)));
    }

    // Refused means untouched, which is the whole reason the checks are up
    // front rather than inside the loop that places them.
    CHECK(f.layout(f.v1) == before);
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

TEST_CASE("Clip properties are edited through the command stack", "[edit][properties]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const model::ClipId id = f.track(f.v1).clips()[0].id;

    SECTION("transform") {
        model::Transform transform;
        transform.positionX = 120.0;
        transform.scaleX = 1.5;
        transform.scaleY = 1.5;
        transform.rotationDegrees = 12.0;
        transform.opacity = 0.4;
        REQUIRE(f.run(edit::makeSetTransform(f.project, f.on(f.v1), id, transform)));

        const model::Clip* clip = f.track(f.v1).find(id);
        REQUIRE(clip != nullptr);
        CHECK(clip->transform == transform);
        // A property change must not move the clip.
        CHECK(clip->start() == f.at(0));
        CHECK(clip->duration() == f.at(50));

        SECTION("and undo restores the previous transform") {
            REQUIRE(f.stack.undo(f.project));
            CHECK(f.track(f.v1).find(id)->transform.isIdentity());
        }
    }

    SECTION("blend mode, with the reason in the history") {
        REQUIRE(f.run(edit::makeSetBlendMode(f.project, f.on(f.v1), id, model::BlendMode::Screen)));
        CHECK(f.track(f.v1).find(id)->blend == model::BlendMode::Screen);
        CHECK(f.stack.undoDescription() == "Set blend mode to screen");
    }

    SECTION("audio gain and pan") {
        REQUIRE(f.run(edit::makeSetClipAudio(f.project, f.on(f.v1), id, -6.0, -0.5)));
        const model::Clip* clip = f.track(f.v1).find(id);
        CHECK(clip->gainDb == -6.0);
        CHECK(clip->pan == -0.5);
    }

    SECTION("pan is clamped rather than refused") {
        REQUIRE(f.run(edit::makeSetClipAudio(f.project, f.on(f.v1), id, 0.0, 4.0)));
        CHECK(f.track(f.v1).find(id)->pan == 1.0);
    }

    SECTION("a gain that is not a number is refused") {
        CHECK_FALSE(f.run(edit::makeSetClipAudio(f.project, f.on(f.v1), id,
                                                 std::numeric_limits<double>::quiet_NaN(), 0.0)));
    }

    SECTION("enabling and disabling") {
        REQUIRE(f.run(edit::makeSetClipEnabled(f.project, f.on(f.v1), id, false)));
        CHECK_FALSE(f.track(f.v1).find(id)->enabled);
        CHECK(f.stack.undoDescription() == "Disable clip");
        // The clip keeps its place; it just stops contributing.
        CHECK(f.track(f.v1).clips().size() == 1);
        CHECK(f.track(f.v1).find(id)->start() == f.at(0));
    }
}

TEST_CASE("Dragging a property slider is one undo step", "[edit][properties]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const model::ClipId id = f.track(f.v1).clips()[0].id;
    const std::size_t before = f.stack.depth();

    // What dragging an opacity slider actually emits.
    for (int step = 1; step <= 100; ++step) {
        model::Transform transform;
        transform.opacity = step / 100.0;
        REQUIRE(f.run(edit::makeSetTransform(f.project, f.on(f.v1), id, transform)));
    }
    CHECK(f.stack.depth() == before + 1);
    CHECK(f.track(f.v1).find(id)->transform.opacity == 1.0);

    REQUIRE(f.stack.undo(f.project));
    CHECK(f.track(f.v1).find(id)->transform.isIdentity());
}

TEST_CASE("Toggling enabled twice is two undo steps", "[edit][properties]") {
    // Unlike a slider drag, two toggles are two decisions rather than one
    // gesture, so they must not coalesce.
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const model::ClipId id = f.track(f.v1).clips()[0].id;
    const std::size_t before = f.stack.depth();

    REQUIRE(f.run(edit::makeSetClipEnabled(f.project, f.on(f.v1), id, false)));
    REQUIRE(f.run(edit::makeSetClipEnabled(f.project, f.on(f.v1), id, true)));
    CHECK(f.stack.depth() == before + 2);
}

TEST_CASE("Property edits respect a locked track", "[edit][properties]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const model::ClipId id = f.track(f.v1).clips()[0].id;
    f.track(f.v1).setLocked(true);

    model::Transform transform;
    transform.opacity = 0.5;
    CHECK_FALSE(f.run(edit::makeSetTransform(f.project, f.on(f.v1), id, transform)));
    CHECK(f.lastError.find("locked") != std::string::npos);
    CHECK(f.track(f.v1).find(id)->transform.isIdentity());
}

TEST_CASE("Property edits survive a round trip through the project file",
          "[edit][properties][io]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 50))));
    const model::ClipId video = f.track(f.v1).clips()[0].id;
    const model::ClipId audio = f.track(f.a1).clips()[0].id;

    model::Transform transform;
    transform.positionX = -40.0;
    transform.scaleY = 0.75;
    transform.rotationDegrees = -9.5;
    transform.opacity = 0.6;
    REQUIRE(f.run(edit::makeSetTransform(f.project, f.on(f.v1), video, transform)));
    REQUIRE(f.run(edit::makeSetBlendMode(f.project, f.on(f.v1), video, model::BlendMode::Add)));
    REQUIRE(f.run(edit::makeSetClipAudio(f.project, f.on(f.a1), audio, -3.5, 0.25)));

    const auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    const auto loaded = io::loadProjectFromString(*text);
    REQUIRE(loaded);
    CHECK(loaded->project == f.project);

    const model::Sequence* sequence = loaded->project.findSequence(f.sequenceId);
    REQUIRE(sequence != nullptr);
    const model::Clip* videoClip = sequence->findTrack(f.v1)->find(video);
    const model::Clip* audioClip = sequence->findTrack(f.a1)->find(audio);
    REQUIRE(videoClip != nullptr);
    REQUIRE(audioClip != nullptr);

    CHECK(videoClip->transform == transform);
    CHECK(videoClip->blend == model::BlendMode::Add);
    CHECK(audioClip->gainDb == -3.5);
    CHECK(audioClip->pan == 0.25);
}

TEST_CASE("Cross dissolves need a cut and handles either side", "[edit][transition]") {
    Fixture f;
    // Both clips start well inside their media, so there is material to reach
    // into on both sides of the cut.
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 600))));

    SECTION("added across the cut, centred on it") {
        REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(50), f.at(10))));
        REQUIRE(f.track(f.v1).transitions().size() == 1);

        const model::Transition& transition = f.track(f.v1).transitions().front();
        CHECK(transition.range.start() == f.at(45));
        CHECK(transition.range.duration() == f.at(10));
        CHECK(transition.from == f.track(f.v1).clips()[0].id);
        CHECK(transition.to == f.track(f.v1).clips()[1].id);

        SECTION("and the clips are untouched: a transition is not an overlap") {
            CHECK(f.layout(f.v1) == "0-50@500 50-100@600");
        }

        SECTION("and it can be removed") {
            REQUIRE(f.run(edit::makeRemoveTransition(f.project, f.on(f.v1), transition.id)));
            CHECK(f.track(f.v1).transitions().empty());
        }

        SECTION("and undo takes it away") {
            REQUIRE(f.stack.undo(f.project));
            CHECK(f.track(f.v1).transitions().empty());
        }
    }

    SECTION("a point near the cut still finds it") {
        REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(47), f.at(8))));
        CHECK(f.track(f.v1).transitions().front().range.start() == f.at(46));
    }

    SECTION("adding a second across the same cut replaces the first") {
        REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(50), f.at(10))));
        REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(50), f.at(20))));
        REQUIRE(f.track(f.v1).transitions().size() == 1);
        CHECK(f.track(f.v1).transitions().front().range.duration() == f.at(20));
    }

    SECTION("one that would start before the sequence is refused") {
        // Centred on frame 50, a 300-frame dissolve would begin at -100.
        CHECK_FALSE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(50), f.at(300))));
        CHECK(f.lastError.find("before the sequence") != std::string::npos);
    }

    SECTION("zero duration is refused") {
        CHECK_FALSE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(50), f.at(0))));
    }
}

TEST_CASE("A dissolve can be stretched by its edges", "[edit][transition]") {
    // What dragging a dissolve's edge on the timeline calls. Re-adding one
    // instead would recentre the span on the cut, which is exactly what a drag
    // of a single edge must not do.
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 600))));
    REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(50), f.at(10))));
    const model::TransitionId id = f.track(f.v1).transitions().front().id;

    SECTION("longer, still centred") {
        REQUIRE(f.run(edit::makeSetTransitionRange(f.project, f.on(f.v1), id,
                                                   time::TimeRange{f.at(40), f.at(20)})));
        const model::Transition& stretched = f.track(f.v1).transitions().front();
        CHECK(stretched.range.start() == f.at(40));
        CHECK(stretched.range.duration() == f.at(20));
        // The clips do not move: a transition is still not an overlap.
        CHECK(f.layout(f.v1) == "0-50@500 50-100@600");
    }

    SECTION("asymmetric, which is how a fade is shaped") {
        // Starting on the cut and running into the incoming clip.
        REQUIRE(f.run(edit::makeSetTransitionRange(f.project, f.on(f.v1), id,
                                                   time::TimeRange{f.at(50), f.at(15)})));
        CHECK(f.track(f.v1).transitions().front().range.start() == f.at(50));
        CHECK(f.track(f.v1).transitions().front().range.duration() == f.at(15));
    }

    SECTION("dragged clear of its cut, refused") {
        CHECK_FALSE(f.run(edit::makeSetTransitionRange(f.project, f.on(f.v1), id,
                                                       time::TimeRange{f.at(10), f.at(10)})));
        CHECK(f.lastError.find("across its cut") != std::string::npos);
    }

    SECTION("longer than the clips it joins, refused") {
        CHECK_FALSE(f.run(edit::makeSetTransitionRange(f.project, f.on(f.v1), id,
                                                       time::TimeRange{f.at(40), f.at(80)})));
        CHECK(f.lastError.find("longer than the clips") != std::string::npos);
    }

    SECTION("zero length, refused") {
        CHECK_FALSE(f.run(edit::makeSetTransitionRange(f.project, f.on(f.v1), id,
                                                       time::TimeRange{f.at(50), f.at(0)})));
    }

    SECTION("a drag is one undo step, not one per frame") {
        const std::size_t before = f.stack.position();
        for (std::int64_t span = 11; span <= 20; ++span) {
            REQUIRE(f.run(edit::makeSetTransitionRange(
                f.project, f.on(f.v1), id, time::TimeRange{f.at(50 - (span / 2)), f.at(span)})));
        }
        CHECK(f.stack.position() == before + 1);
        CHECK(f.track(f.v1).transitions().front().range.duration() == f.at(20));
    }
}

TEST_CASE("A dissolve longer than the clips it joins is refused", "[edit][transition]") {
    // Placed far enough along that the span does not run off the front of the
    // sequence, so it is the clip length that binds rather than frame zero.
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(200, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(250, 50, 600))));

    CHECK_FALSE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(250), f.at(120))));
    CHECK(f.lastError.find("longer than the clips") != std::string::npos);

    // Something that fits is still accepted.
    CHECK(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(250), f.at(40))));
}

TEST_CASE("A dissolve is refused where there are no handles", "[edit][transition]") {
    // The outgoing clip runs to the very last frame of its media, so there is
    // nothing to show past the cut. Silently shortening the dissolve or filling
    // with black would both be worse than saying so.
    Fixture f;
    model::Clip first = f.clip(0, Fixture::kShortMediaFrames, 0, f.shortMedia);
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), first)));
    REQUIRE(f.run(
        edit::makeOverwrite(f.project, f.on(f.v1), f.clip(Fixture::kShortMediaFrames, 50, 600))));

    CHECK_FALSE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1),
                                                 f.at(Fixture::kShortMediaFrames), f.at(10))));
    CHECK(f.lastError.find("no handles") != std::string::npos);
}

TEST_CASE("A gap is not a cut: it fades instead", "[edit][transition]") {
    // A gap has no second clip to dissolve into, so there is no crossfade to
    // make -- but both of its sides are a clip ending or starting against
    // nothing, which is exactly what a fade is. Asking here used to be refused
    // outright; it now gives the thing that was actually wanted.
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(80, 50, 600))));
    REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(65), f.at(10))));

    REQUIRE(f.track(f.v1).transitions().size() == 1);
    const model::Transition& fade = f.track(f.v1).transitions().front();
    CHECK(fade.isFadeOut());
    CHECK_FALSE(fade.isCrossFade());
    CHECK(fade.from == f.track(f.v1).clips()[0].id);
    // Inside the clip and anchored to the end it fades at, rather than
    // straddling anything.
    CHECK(fade.range.endExclusive() == f.at(50));
    CHECK(fade.range.duration() == f.at(10));
}

TEST_CASE("The dissolve button fades a clip that has nothing beside it", "[edit][transition]") {
    Fixture f;

    SECTION("at the tail, a fade out") {
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
        REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(50), f.at(12))));
        const model::Transition& fade = f.track(f.v1).transitions().front();
        CHECK(fade.isFadeOut());
        CHECK(fade.range == time::TimeRange{f.at(38), f.at(12)});
    }

    SECTION("at the head, a fade in") {
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
        REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(0), f.at(12))));
        const model::Transition& fade = f.track(f.v1).transitions().front();
        CHECK(fade.isFadeIn());
        CHECK(fade.range == time::TimeRange{f.at(0), f.at(12)});
    }

    SECTION("a clip using every frame of its source can still fade") {
        // The whole point of a fade lying inside its clip: it reads no handles,
        // so the material a crossfade would need is material it never asks for.
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1),
                                          f.clip(0, Fixture::kShortMediaFrames, 0, f.shortMedia))));
        REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1),
                                                 f.at(Fixture::kShortMediaFrames), f.at(10))));
        CHECK(f.track(f.v1).transitions().front().isFadeOut());
    }

    SECTION("longer than the clip it is on, refused") {
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
        CHECK_FALSE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(50), f.at(80))));
        CHECK(f.lastError.find("longer than the clip") != std::string::npos);
    }

    SECTION("a cut nearer than the free end still wins") {
        // Two clips meeting at 50, the run ending at 100. Asked at 48, the cut
        // is two frames away and the tail fifty-two: a crossfade, not a fade.
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 600))));
        REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(48), f.at(10))));
        CHECK(f.track(f.v1).transitions().front().isCrossFade());
    }

    SECTION("and the free end wins when it is the nearer one") {
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 600))));
        REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(99), f.at(10))));
        const model::Transition& fade = f.track(f.v1).transitions().front();
        CHECK(fade.isFadeOut());
        CHECK(fade.range.endExclusive() == f.at(100));
    }
}

TEST_CASE("A fade is resized from its free edge only", "[edit][transition]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(50), f.at(10))));
    const model::TransitionId id = f.track(f.v1).transitions().front().id;

    SECTION("dragging the free edge sets the length") {
        REQUIRE(f.run(edit::makeSetTransitionRange(f.project, f.on(f.v1), id,
                                                   time::TimeRange{f.at(20), f.at(30)})));
        CHECK(f.track(f.v1).transitions().front().range == time::TimeRange{f.at(20), f.at(30)});
    }

    SECTION("moving the anchored end is refused") {
        // A fade out that stops before the clip does is not a shorter fade,
        // it is a fade somewhere in the middle of a shot.
        CHECK_FALSE(f.run(edit::makeSetTransitionRange(f.project, f.on(f.v1), id,
                                                       time::TimeRange{f.at(30), f.at(10)})));
        CHECK(f.lastError.find("stays at the end") != std::string::npos);
    }

    SECTION("longer than its clip is refused") {
        CHECK_FALSE(f.run(edit::makeSetTransitionRange(f.project, f.on(f.v1), id,
                                                       time::TimeRange{f.at(-10), f.at(60)})));
    }
}

TEST_CASE("Transitions and track gain survive a round trip", "[edit][transition][io]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 600))));
    REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(50), f.at(12))));

    // Track gain and pan were silently dropped on save: the decoder read them,
    // the encoder never wrote them, and no test had ever set them.
    f.track(f.a1).setGainDb(-4.5);
    f.track(f.a1).setPan(0.75);

    const auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    const auto loaded = io::loadProjectFromString(*text);
    REQUIRE(loaded);
    CHECK(loaded->project == f.project);

    const model::Sequence* sequence = loaded->project.findSequence(f.sequenceId);
    REQUIRE(sequence != nullptr);

    const model::Track* video = sequence->findTrack(f.v1);
    REQUIRE(video != nullptr);
    REQUIRE(video->transitions().size() == 1);
    CHECK(video->transitions().front().range.start() == f.at(44));
    CHECK(video->transitions().front().kind == model::TransitionKind::CrossDissolve);

    const model::Track* audio = sequence->findTrack(f.a1);
    REQUIRE(audio != nullptr);
    CHECK(audio->gainDb() == -4.5);
    CHECK(audio->pan() == 0.75);
}

TEST_CASE("Importing media goes through the command stack", "[edit][import]") {
    Fixture f;
    const std::size_t before = f.project.media().size();

    model::MediaRef ref;
    ref.path = "/media/new-take.mov";
    ref.name = "new-take.mov";
    ref.info.duration = time::Rational{30, 1};

    REQUIRE(f.run(edit::makeImportMedia(f.project, ref)));
    CHECK(f.project.media().size() == before + 1);
    CHECK(f.stack.undoDescription() == "Import new-take.mov");

    SECTION("and undo removes it again") {
        REQUIRE(f.stack.undo(f.project));
        CHECK(f.project.media().size() == before);

        SECTION("without ever reissuing an id something already used") {
            // Snapshot restore would otherwise roll the counter back, and the
            // next import would hand out an id that the undone one had taken.
            const auto next = f.project.ids().next<model::MediaRefTag>();
            for (const model::MediaRef& existing : f.project.media()) {
                CHECK(existing.id != next);
            }
            REQUIRE(f.stack.redo(f.project));
            for (const model::MediaRef& existing : f.project.media()) {
                CHECK(existing.id != next);
            }
        }
    }

    SECTION("media with no path is refused") {
        model::MediaRef empty;
        CHECK_FALSE(f.run(edit::makeImportMedia(f.project, empty)));
    }

    SECTION("the sequence is untouched by an import") {
        CHECK(f.track(f.v1).isEmpty());
    }
}

TEST_CASE("Three-point editing places a marked range on the timeline", "[edit][threepoint]") {
    Fixture f;

    SECTION("overwrite at the playhead") {
        REQUIRE(
            f.run(edit::makePlaceFromSource(f.project, f.on(f.v1), f.longMedia, f.range(200, 50),
                                            f.at(100), edit::PlaceMode::Overwrite)));
        CHECK(f.layout(f.v1) == "100-150@200");
    }

    SECTION("insert pushes what follows") {
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 100, 500))));
        REQUIRE(
            f.run(edit::makePlaceFromSource(f.project, f.on(f.v1), f.longMedia, f.range(200, 20),
                                            f.at(40), edit::PlaceMode::Insert)));
        // The clip under the point is split and the tail moves along.
        CHECK(f.layout(f.v1) == "0-40@500 40-60@200 60-120@540");
    }

    SECTION("the duration comes from the marked range, not from the caller") {
        REQUIRE(f.run(edit::makePlaceFromSource(f.project, f.on(f.v1), f.longMedia, f.range(0, 37),
                                                f.at(0), edit::PlaceMode::Overwrite)));
        CHECK(f.track(f.v1).clips()[0].duration() == f.at(37));
    }

    SECTION("an unmarked range is refused") {
        CHECK_FALSE(
            f.run(edit::makePlaceFromSource(f.project, f.on(f.v1), f.longMedia, f.range(200, 0),
                                            f.at(0), edit::PlaceMode::Overwrite)));
        CHECK(f.lastError.find("in and an out") != std::string::npos);
    }

    SECTION("a range that runs past the end of the media is refused") {
        CHECK_FALSE(f.run(edit::makePlaceFromSource(f.project, f.on(f.v1), f.shortMedia,
                                                    f.range(Fixture::kShortMediaFrames - 5, 50),
                                                    f.at(0), edit::PlaceMode::Overwrite)));
    }

    SECTION("media that is not in the project is refused") {
        CHECK_FALSE(
            f.run(edit::makePlaceFromSource(f.project, f.on(f.v1), model::MediaRefId{9999},
                                            f.range(0, 10), f.at(0), edit::PlaceMode::Overwrite)));
    }
}

TEST_CASE("A marked range converts between source and sequence rates", "[edit][threepoint]") {
    // Twenty-four frames of a 24fps take is one second, which on this 25fps
    // timeline is twenty-five frames. Copying the frame count across would put
    // every edit assembled from mixed-rate media a frame short per second.
    Fixture f;
    const time::Rational sourceRate = time::rates::fps24;
    const time::TimeRange marked{time::RationalTime{48, sourceRate},
                                 time::RationalTime{24, sourceRate}};

    REQUIRE(f.run(edit::makePlaceFromSource(f.project, f.on(f.v1), f.longMedia, marked, f.at(0),
                                            edit::PlaceMode::Overwrite)));

    const model::Clip& clip = f.track(f.v1).clips()[0];
    CHECK(clip.duration() == f.at(25));
    // And the source range keeps its own rate, so decoding still asks the right
    // questions of the file.
    CHECK(clip.sourceRange.start().rate() == sourceRate);
    CHECK(clip.sourceRange.start().frames() == 48);
}

namespace {

/// A linked picture-and-sound pair, the arrangement everything below is about.
struct LinkedPair {
    model::ClipId video;
    model::ClipId audio;
};

LinkedPair linkPair(testing::Fixture& f, std::int64_t start, std::int64_t length) {
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(start, length, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(start, length, 500))));
    const model::ClipId video = f.track(f.v1).clipAt(f.at(start))->id;
    const model::ClipId audio = f.track(f.a1).clipAt(f.at(start))->id;
    REQUIRE(f.run(edit::makeLinkClips(f.project, f.sequenceId, {{f.v1, video}, {f.a1, audio}})));
    return {video, audio};
}

}  // namespace

TEST_CASE("Linked clips move together", "[edit][link]") {
    testing::Fixture f;
    const LinkedPair pair = linkPair(f, 0, 50);

    REQUIRE(f.run(edit::makeMove(f.project, f.on(f.v1), pair.video, f.v1, f.at(200))));
    CHECK(f.layout(f.v1) == "200-250@500");
    // Sound follows picture rather than joining it: it moves by the same
    // amount and stays on its own track.
    CHECK(f.layout(f.a1) == "200-250@500");

    SECTION("and one undo puts both back") {
        REQUIRE(f.stack.undo(f.project));
        CHECK(f.layout(f.v1) == "0-50@500");
        CHECK(f.layout(f.a1) == "0-50@500");
    }
}

TEST_CASE("Linked clips are removed together", "[edit][link]") {
    testing::Fixture f;
    const LinkedPair pair = linkPair(f, 0, 50);

    SECTION("lift") {
        REQUIRE(f.run(edit::makeLift(f.project, f.on(f.v1), pair.video)));
        CHECK(f.track(f.v1).isEmpty());
        CHECK(f.track(f.a1).isEmpty());
    }

    SECTION("extract") {
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 600))));
        REQUIRE(f.run(edit::makeExtract(f.project, f.on(f.v1), pair.video)));
        CHECK(f.layout(f.v1) == "0-50@600");
        CHECK(f.track(f.a1).isEmpty());
    }
}

TEST_CASE("Linked clips are cut together", "[edit][link][razor]") {
    testing::Fixture f;
    static_cast<void>(linkPair(f, 0, 50));

    REQUIRE(f.run(edit::makeRazor(f.project, f.on(f.v1), f.at(20))));
    CHECK(f.layout(f.v1) == "0-20@500 20-50@520");
    // The point of the whole thing: the sound is cut in the same place, so the
    // two halves can be moved apart without one of them dragging the other
    // half's audio along.
    CHECK(f.layout(f.a1) == "0-20@500 20-50@520");

    SECTION("and the halves are two link groups, not one of four") {
        const auto& video = f.track(f.v1).clips();
        const auto& audio = f.track(f.a1).clips();
        REQUIRE(video.size() == 2);
        REQUIRE(audio.size() == 2);
        CHECK(video[0].link == audio[0].link);
        CHECK(video[1].link == audio[1].link);
        CHECK(video[0].link != video[1].link);
        CHECK(video[0].link.isValid());
        CHECK(video[1].link.isValid());
    }

    SECTION("so moving one half leaves the other where it was") {
        const model::ClipId tail = f.track(f.v1).clips()[1].id;
        REQUIRE(f.run(edit::makeMove(f.project, f.on(f.v1), tail, f.v1, f.at(200))));
        CHECK(f.layout(f.v1) == "0-20@500 200-230@520");
        CHECK(f.layout(f.a1) == "0-20@500 200-230@520");
    }

    SECTION("and one undo puts both tracks back") {
        REQUIRE(f.stack.undo(f.project));
        CHECK(f.layout(f.v1) == "0-50@500");
        CHECK(f.layout(f.a1) == "0-50@500");
    }
}

TEST_CASE("A cut that misses a linked partner only cuts what it is inside", "[edit][link][razor]") {
    testing::Fixture f;
    // Sound that stops before the picture does -- a link group's clips need not
    // span the same range.
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 20, 500))));
    const model::ClipId video = f.track(f.v1).clipAt(f.at(0))->id;
    const model::ClipId audio = f.track(f.a1).clipAt(f.at(0))->id;
    REQUIRE(f.run(edit::makeLinkClips(f.project, f.sequenceId, {{f.v1, video}, {f.a1, audio}})));

    REQUIRE(f.run(edit::makeRazor(f.project, f.on(f.v1), f.at(30))));
    CHECK(f.layout(f.v1) == "0-30@500 30-50@530");
    // Past the end of the sound, so the sound is left alone rather than
    // refused or cut at its own end.
    CHECK(f.layout(f.a1) == "0-20@500");
}

TEST_CASE("Linked clips trim together", "[edit][link]") {
    testing::Fixture f;
    const LinkedPair pair = linkPair(f, 0, 50);

    REQUIRE(f.run(edit::makeTrim(f.project, f.on(f.v1), pair.video, edit::Edge::Out, f.at(-10))));
    CHECK(f.layout(f.v1) == "0-40@500");
    CHECK(f.layout(f.a1) == "0-40@500");
}

TEST_CASE("Unlinking leaves the clips standing alone", "[edit][link]") {
    testing::Fixture f;
    const LinkedPair pair = linkPair(f, 0, 50);
    REQUIRE(f.run(edit::makeUnlinkClips(f.project, f.on(f.v1), pair.video)));

    REQUIRE(f.run(edit::makeMove(f.project, f.on(f.v1), pair.video, f.v1, f.at(200))));
    CHECK(f.layout(f.v1) == "200-250@500");
    CHECK(f.layout(f.a1) == "0-50@500");  // stayed put

    SECTION("and unlinking something unlinked is refused") {
        CHECK_FALSE(f.run(edit::makeUnlinkClips(f.project, f.on(f.v1), pair.video)));
    }
}

TEST_CASE("Linking needs at least two clips", "[edit][link]") {
    testing::Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const model::ClipId id = f.track(f.v1).clips()[0].id;
    CHECK_FALSE(f.run(edit::makeLinkClips(f.project, f.sequenceId, {{f.v1, id}})));
}

TEST_CASE("A locked track keeps its clips where they are, even when linked", "[edit][link]") {
    // Refusing the whole edit instead would let one locked track block editing
    // everywhere it happens to be linked.
    testing::Fixture f;
    const LinkedPair pair = linkPair(f, 0, 50);
    f.track(f.a1).setLocked(true);

    REQUIRE(f.run(edit::makeMove(f.project, f.on(f.v1), pair.video, f.v1, f.at(200))));
    CHECK(f.layout(f.v1) == "200-250@500");
    CHECK(f.layout(f.a1) == "0-50@500");
}

TEST_CASE("A track with sync lock off does not follow a ripple", "[edit][synclock]") {
    testing::Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 600))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 200, 700))));

    SECTION("with sync lock on, everything shifts") {
        REQUIRE(f.run(edit::makeRippleDelete(f.project, f.on(f.v1), f.range(0, 50), true)));
        CHECK(f.layout(f.v1) == "0-50@600");
        CHECK(f.layout(f.a1) == "0-150@750");
    }

    SECTION("with it off, the music bed stays where it is") {
        f.track(f.a1).setSyncLocked(false);
        REQUIRE(f.run(edit::makeRippleDelete(f.project, f.on(f.v1), f.range(0, 50), true)));
        CHECK(f.layout(f.v1) == "0-50@600");
        // Untouched: the range was not cleared from it and it did not shift.
        CHECK(f.layout(f.a1) == "0-200@700");
    }
}

TEST_CASE("Links and sync locks survive a round trip", "[edit][link][io]") {
    testing::Fixture f;
    const LinkedPair pair = linkPair(f, 0, 50);
    f.track(f.a1).setSyncLocked(false);

    const auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    const auto loaded = io::loadProjectFromString(*text);
    REQUIRE(loaded);
    CHECK(loaded->project == f.project);

    const model::Sequence* sequence = loaded->project.findSequence(f.sequenceId);
    REQUIRE(sequence != nullptr);
    const model::Clip* video = sequence->findTrack(f.v1)->find(pair.video);
    const model::Clip* audio = sequence->findTrack(f.a1)->find(pair.audio);
    REQUIRE(video != nullptr);
    REQUIRE(audio != nullptr);

    CHECK(video->link.isValid());
    CHECK(video->link == audio->link);
    CHECK_FALSE(sequence->findTrack(f.a1)->isSyncLocked());
    CHECK(sequence->findTrack(f.v1)->isSyncLocked());
}

TEST_CASE("Markers are added, found and navigated", "[edit][marker]") {
    testing::Fixture f;

    REQUIRE(f.run(edit::makeAddMarker(f.project, f.sequenceId, f.at(100), f.at(0), "Take 2")));
    REQUIRE(f.run(edit::makeAddMarker(f.project, f.sequenceId, f.at(50), f.at(25), "Section")));
    REQUIRE(f.run(edit::makeAddMarker(f.project, f.sequenceId, f.at(300), f.at(0), "End")));

    const model::Sequence& sequence = f.sequence();
    REQUIRE(sequence.markers().size() == 3);

    SECTION("and kept in time order however they were added") {
        CHECK(sequence.markers()[0].range.start() == f.at(50));
        CHECK(sequence.markers()[1].range.start() == f.at(100));
        CHECK(sequence.markers()[2].range.start() == f.at(300));
    }

    SECTION("a zero duration becomes a one-frame point") {
        const model::Marker* point = sequence.markerAt(f.at(100));
        REQUIRE(point != nullptr);
        CHECK(point->name == "Take 2");
        CHECK(point->isPoint());
        CHECK(point->range.duration() == f.at(1));
        // And it is findable at its own frame, which an empty range would not be.
        CHECK(sequence.markerAt(f.at(101)) == nullptr);
    }

    SECTION("a spanned marker covers its whole range") {
        CHECK(sequence.markerAt(f.at(50)) != nullptr);
        CHECK(sequence.markerAt(f.at(74)) != nullptr);
        CHECK(sequence.markerAt(f.at(75)) == nullptr);
    }

    SECTION("jumping forward and back") {
        REQUIRE(sequence.markerAfter(f.at(0)) != nullptr);
        CHECK(sequence.markerAfter(f.at(0))->range.start() == f.at(50));
        CHECK(sequence.markerAfter(f.at(50))->range.start() == f.at(100));
        CHECK(sequence.markerAfter(f.at(300)) == nullptr);

        CHECK(sequence.markerBefore(f.at(300))->range.start() == f.at(100));
        CHECK(sequence.markerBefore(f.at(0)) == nullptr);
    }

    SECTION("removing one") {
        const model::MarkerId id = sequence.markers()[1].id;
        REQUIRE(f.run(edit::makeRemoveMarker(f.project, f.sequenceId, id)));
        CHECK(f.sequence().markers().size() == 2);
        CHECK(f.sequence().markerAt(f.at(100)) == nullptr);

        SECTION("and undo brings it back") {
            REQUIRE(f.stack.undo(f.project));
            CHECK(f.sequence().markers().size() == 3);
        }
    }

    SECTION("renaming one, coalescing while typing") {
        const model::MarkerId id = sequence.markers()[0].id;
        const std::size_t before = f.stack.depth();
        for (const char* name : {"S", "Se", "Sec", "Sect"}) {
            REQUIRE(f.run(edit::makeUpdateMarker(f.project, f.sequenceId, id, name, "", 2)));
        }
        CHECK(f.stack.depth() == before + 1);
        CHECK(f.sequence().markers()[0].name == "Sect");
        CHECK(f.sequence().markers()[0].colour == 2);
    }

    SECTION("removing one that is not there is refused") {
        CHECK_FALSE(f.run(edit::makeRemoveMarker(f.project, f.sequenceId, model::MarkerId{999})));
    }

    SECTION("a marker before the sequence is refused") {
        CHECK_FALSE(f.run(edit::makeAddMarker(f.project, f.sequenceId, f.at(-5), f.at(0), "x")));
    }
}

TEST_CASE("Markers survive a round trip", "[edit][marker][io]") {
    testing::Fixture f;
    REQUIRE(
        f.run(edit::makeAddMarker(f.project, f.sequenceId, f.at(120), f.at(30), "Colour pass", 3)));
    const model::MarkerId id = f.sequence().markers().front().id;
    REQUIRE(f.run(edit::makeUpdateMarker(f.project, f.sequenceId, id, "Colour pass",
                                         "Too warm from here", 3)));

    const auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    const auto loaded = io::loadProjectFromString(*text);
    REQUIRE(loaded);
    CHECK(loaded->project == f.project);

    const model::Sequence* sequence = loaded->project.findSequence(f.sequenceId);
    REQUIRE(sequence != nullptr);
    REQUIRE(sequence->markers().size() == 1);
    const model::Marker& marker = sequence->markers().front();
    CHECK(marker.id == id);
    CHECK(marker.name == "Colour pass");
    CHECK(marker.note == "Too warm from here");
    CHECK(marker.colour == 3);
    CHECK(marker.range.start() == f.at(120));
    CHECK(marker.range.duration() == f.at(30));
}

TEST_CASE("Several clips move as one", "[edit][multiselect]") {
    testing::Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 600))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(200, 50, 700))));

    const auto& clips = f.track(f.v1).clips();
    const std::vector<edit::ClipRef> selection{{f.v1, clips[0].id}, {f.v1, clips[1].id}};

    SECTION("keeping their spacing") {
        REQUIRE(f.run(edit::makeMoveClips(f.project, f.sequenceId, selection, f.at(300))));
        CHECK(f.layout(f.v1) == "200-250@700 300-350@500 350-400@600");
    }

    SECTION("the result does not depend on the order they were given in") {
        testing::Fixture g;
        REQUIRE(g.run(edit::makeOverwrite(g.project, g.on(g.v1), g.clip(0, 50, 500))));
        REQUIRE(g.run(edit::makeOverwrite(g.project, g.on(g.v1), g.clip(50, 50, 600))));
        const auto& other = g.track(g.v1).clips();
        // Reversed: moving one at a time, the first would overwrite the second
        // on its way past.
        REQUIRE(g.run(edit::makeMoveClips(g.project, g.sequenceId,
                                          {{g.v1, other[1].id}, {g.v1, other[0].id}}, g.at(25))));
        CHECK(g.layout(g.v1) == "25-75@500 75-125@600");
    }

    SECTION("moving onto itself is not a collision") {
        // A shift smaller than the clips' own length means the set overlaps
        // where it was, which only works because everything is lifted first.
        REQUIRE(f.run(edit::makeMoveClips(f.project, f.sequenceId, selection, f.at(10))));
        CHECK(f.layout(f.v1) == "10-60@500 60-110@600 200-250@700");
    }

    SECTION("one undo for the whole set") {
        REQUIRE(f.run(edit::makeMoveClips(f.project, f.sequenceId, selection, f.at(300))));
        REQUIRE(f.stack.undo(f.project));
        CHECK(f.layout(f.v1) == "0-50@500 50-100@600 200-250@700");
    }

    SECTION("moving before the start is refused") {
        CHECK_FALSE(f.run(edit::makeMoveClips(f.project, f.sequenceId, selection, f.at(-10))));
    }

    SECTION("an empty selection is refused") {
        CHECK_FALSE(f.run(edit::makeMoveClips(f.project, f.sequenceId, {}, f.at(10))));
    }
}

TEST_CASE("Several clips are removed as one", "[edit][multiselect]") {
    testing::Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 600))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(100, 50, 700))));

    const auto& clips = f.track(f.v1).clips();
    const std::vector<edit::ClipRef> selection{{f.v1, clips[0].id}, {f.v1, clips[2].id}};

    SECTION("lifting leaves the gaps") {
        REQUIRE(f.run(edit::makeRemoveClips(f.project, f.sequenceId, selection, false)));
        CHECK(f.layout(f.v1) == "50-100@600");
    }

    SECTION("extracting closes them, latest first") {
        // Closing the earlier gap first would move the later clip out from
        // under the position recorded for it.
        REQUIRE(f.run(edit::makeRemoveClips(f.project, f.sequenceId, selection, true)));
        CHECK(f.layout(f.v1) == "0-50@600");
    }
}

TEST_CASE("A multi-clip edit carries linked partners with it", "[edit][multiselect][link]") {
    testing::Fixture f;
    const LinkedPair first = linkPair(f, 0, 50);
    const LinkedPair second = linkPair(f, 50, 50);

    // Only the picture is selected; the sound comes along because it is linked.
    REQUIRE(f.run(edit::makeMoveClips(f.project, f.sequenceId,
                                      {{f.v1, first.video}, {f.v1, second.video}}, f.at(200))));
    CHECK(f.layout(f.v1) == "200-250@500 250-300@500");
    CHECK(f.layout(f.a1) == "200-250@500 250-300@500");

    SECTION("and removing them takes the sound too") {
        REQUIRE(
            f.run(edit::makeRemoveClips(f.project, f.sequenceId, {{f.v1, first.video}}, false)));
        CHECK(f.layout(f.v1) == "250-300@500");
        CHECK(f.layout(f.a1) == "250-300@500");
    }
}

TEST_CASE("A multi-clip edit refuses a locked track", "[edit][multiselect]") {
    testing::Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v2), f.clip(0, 50, 600))));
    const model::ClipId a = f.track(f.v1).clips()[0].id;
    const model::ClipId b = f.track(f.v2).clips()[0].id;
    f.track(f.v2).setLocked(true);

    // Refused outright rather than moving half the selection: the user pointed
    // at a set, and half of it arriving somewhere else is worse than nothing.
    CHECK_FALSE(
        f.run(edit::makeMoveClips(f.project, f.sequenceId, {{f.v1, a}, {f.v2, b}}, f.at(100))));
    CHECK(f.layout(f.v1) == "0-50@500");
    CHECK(f.layout(f.v2) == "0-50@600");
}

TEST_CASE("The stopwatch starts animation without changing the picture", "[edit][keyframes]") {
    Fixture f;
    model::Clip placed = f.clip(100, 50, 500);
    placed.transform.opacity = 0.4;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), placed)));

    REQUIRE(f.run(edit::makeSetParameterAnimated(f.project, f.on(f.v1), placed.id,
                                                 model::Param::Opacity, true, f.at(120))));

    const model::Clip* clip = f.track(f.v1).find(placed.id);
    REQUIRE(clip != nullptr);
    const model::Curve* curve = clip->animation.find(model::Param::Opacity);
    REQUIRE(curve != nullptr);
    // One keyframe, holding what the parameter already showed. Anything else
    // would move the picture at the moment the stopwatch was pressed.
    REQUIRE(curve->size() == 1);
    CHECK(curve->keyframes().front().value == Approx(0.4));
    CHECK(clip->transformAt(f.at(120)).opacity == Approx(0.4));
    CHECK(clip->transformAt(f.at(101)).opacity == Approx(0.4));

    // The keyframe lands at the playhead, in source time.
    CHECK(curve->keyframes().front().time == clip->sourceTimeAt(f.at(120)));
}

TEST_CASE("Stopping animation keeps the value that was on screen", "[edit][keyframes]") {
    // Reverting to the static value underneath would make the picture jump at
    // the instant animation was switched off, and that value is usually the
    // default rather than anything anyone chose.
    Fixture f;
    model::Clip placed = f.clip(100, 50, 500);
    placed.transform.opacity = 1.0;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), placed)));

    const auto source = [&](std::int64_t timelineFrame) {
        return f.track(f.v1).find(placed.id)->sourceTimeAt(f.at(timelineFrame));
    };
    REQUIRE(f.run(edit::makeSetKeyframe(f.project, f.on(f.v1), placed.id, model::Param::Opacity,
                                        source(100), 0.0)));
    REQUIRE(f.run(edit::makeSetKeyframe(f.project, f.on(f.v1), placed.id, model::Param::Opacity,
                                        source(120), 1.0)));
    const double showing = f.track(f.v1).find(placed.id)->transformAt(f.at(110)).opacity;
    CHECK(showing == Approx(0.5));

    REQUIRE(f.run(edit::makeSetParameterAnimated(f.project, f.on(f.v1), placed.id,
                                                 model::Param::Opacity, false, f.at(110))));

    const model::Clip* clip = f.track(f.v1).find(placed.id);
    CHECK(clip->animation.find(model::Param::Opacity) == nullptr);
    CHECK(clip->transform.opacity == Approx(showing));
    // And now it is that value everywhere, because nothing is animated.
    CHECK(clip->transformAt(f.at(100)).opacity == Approx(showing));
    CHECK(clip->transformAt(f.at(140)).opacity == Approx(showing));
}

TEST_CASE("Setting a keyframe twice at one time replaces it and keeps its shape",
          "[edit][keyframes]") {
    Fixture f;
    model::Clip placed = f.clip(100, 50, 500);
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), placed)));
    const auto source = f.track(f.v1).find(placed.id)->sourceTimeAt(f.at(110));

    REQUIRE(f.run(edit::makeSetKeyframe(f.project, f.on(f.v1), placed.id, model::Param::ScaleX,
                                        source, 2.0, model::Interpolation::Bezier)));
    REQUIRE(f.run(edit::makeSetKeyframeInterpolation(f.project, f.on(f.v1), placed.id,
                                                     model::Param::ScaleX, source,
                                                     model::Interpolation::Hold)));
    REQUIRE(f.run(edit::makeSetKeyframe(f.project, f.on(f.v1), placed.id, model::Param::ScaleX,
                                        source, 3.0)));

    const model::Curve* curve = f.track(f.v1).find(placed.id)->animation.find(model::Param::ScaleX);
    REQUIRE(curve != nullptr);
    REQUIRE(curve->size() == 1);
    CHECK(curve->keyframes().front().value == Approx(3.0));
    // Dragging a value must not silently flatten a curve someone shaped.
    CHECK(curve->keyframes().front().interpolation == model::Interpolation::Hold);
}

TEST_CASE("Removing the last keyframe stops the parameter being animated", "[edit][keyframes]") {
    Fixture f;
    model::Clip placed = f.clip(100, 50, 500);
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), placed)));
    const auto source = f.track(f.v1).find(placed.id)->sourceTimeAt(f.at(110));

    REQUIRE(f.run(edit::makeSetKeyframe(f.project, f.on(f.v1), placed.id, model::Param::Opacity,
                                        source, 0.25)));
    REQUIRE(f.run(
        edit::makeRemoveKeyframe(f.project, f.on(f.v1), placed.id, model::Param::Opacity, source)));

    // Not an empty curve left behind saying the parameter is still animated.
    CHECK(f.track(f.v1).find(placed.id)->animation.empty());
    CHECK_FALSE(
        edit::makeRemoveKeyframe(f.project, f.on(f.v1), placed.id, model::Param::Opacity, source));
}

TEST_CASE("A keyframe will not be dragged on top of another", "[edit][keyframes]") {
    Fixture f;
    model::Clip placed = f.clip(100, 50, 500);
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), placed)));
    const model::Clip* clip = f.track(f.v1).find(placed.id);
    const auto first = clip->sourceTimeAt(f.at(105));
    const auto second = clip->sourceTimeAt(f.at(115));

    REQUIRE(f.run(edit::makeSetKeyframe(f.project, f.on(f.v1), placed.id, model::Param::Opacity,
                                        first, 0.0)));
    REQUIRE(f.run(edit::makeSetKeyframe(f.project, f.on(f.v1), placed.id, model::Param::Opacity,
                                        second, 1.0)));

    // Landing on another keyframe would destroy it, and someone who cannot see
    // what was underneath cannot know to undo.
    CHECK_FALSE(edit::makeMoveKeyframe(f.project, f.on(f.v1), placed.id, model::Param::Opacity,
                                       first, second));

    const auto third = clip->sourceTimeAt(f.at(112));
    REQUIRE(f.run(edit::makeMoveKeyframe(f.project, f.on(f.v1), placed.id, model::Param::Opacity,
                                         first, third)));
    const model::Curve* curve =
        f.track(f.v1).find(placed.id)->animation.find(model::Param::Opacity);
    REQUIRE(curve->size() == 2);
    CHECK(curve->at(third) != nullptr);
    CHECK(curve->at(first) == nullptr);
    // Value carried across, and the curve is still sorted.
    CHECK(curve->at(third)->value == Approx(0.0));
    CHECK(curve->keyframes().front().time == third);
}

TEST_CASE("Keyframe edits are refused on a locked track", "[edit][keyframes]") {
    Fixture f;
    model::Clip placed = f.clip(100, 50, 500);
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), placed)));
    const auto source = f.track(f.v1).find(placed.id)->sourceTimeAt(f.at(110));
    f.track(f.v1).setLocked(true);

    CHECK_FALSE(edit::makeSetKeyframe(f.project, f.on(f.v1), placed.id, model::Param::Opacity,
                                      source, 0.5));
    CHECK_FALSE(edit::makeSetParameterAnimated(f.project, f.on(f.v1), placed.id,
                                               model::Param::Opacity, true, f.at(110)));
}

TEST_CASE("Dragging one value coalesces, but two keyframes are two steps", "[edit][keyframes]") {
    Fixture f;
    model::Clip placed = f.clip(100, 50, 500);
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), placed)));
    const model::Clip* clip = f.track(f.v1).find(placed.id);
    const auto first = clip->sourceTimeAt(f.at(105));
    const auto second = clip->sourceTimeAt(f.at(115));

    const std::size_t before = f.stack.depth();
    for (int i = 1; i <= 10; ++i) {
        REQUIRE(f.run(edit::makeSetKeyframe(f.project, f.on(f.v1), placed.id, model::Param::Opacity,
                                            first, i / 10.0)));
    }
    CHECK(f.stack.depth() == before + 1);

    REQUIRE(f.run(edit::makeSetKeyframe(f.project, f.on(f.v1), placed.id, model::Param::Opacity,
                                        second, 0.5)));
    // A second keyframe is a separate decision, not more of the same gesture.
    CHECK(f.stack.depth() == before + 2);
}

TEST_CASE("A speed change with no ripple refuses to run over a neighbour", "[edit][retime]") {
    // `Track::setClips` asserts that clips do not overlap, so a slowed clip
    // growing into the one after it used to be a crash rather than a refusal.
    // Rippling is the default and moves what follows out of the way; the point
    // here is that turning it off is honest about what it cannot do.
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50))));
    const model::ClipId first = f.track(f.v1).clips()[0].id;

    // Half speed is twice as long, and there is a clip butted up against it.
    auto refused = edit::makeSetSpeed(f.project, f.on(f.v1), first, 0.5, false, false);
    CHECK_FALSE(refused);

    // Speeding it up needs no room at all, so that still works.
    REQUIRE(f.run(edit::makeSetSpeed(f.project, f.on(f.v1), first, 2.0, false, false)));
    CHECK(f.track(f.v1).find(first)->timelineRange.duration().frames() == 25);

    // And with the ripple on, slowing it down moves the neighbour instead.
    // On a fresh pair, so what the neighbour ends up at is the length of the
    // clip in front of it rather than the sum of two earlier retimes.
    Fixture g;
    REQUIRE(g.run(edit::makeOverwrite(g.project, g.on(g.v1), g.clip(0, 50))));
    REQUIRE(g.run(edit::makeOverwrite(g.project, g.on(g.v1), g.clip(50, 50))));
    const model::ClipId head = g.track(g.v1).clips()[0].id;
    REQUIRE(g.run(edit::makeSetSpeed(g.project, g.on(g.v1), head, 0.5, false, true)));
    CHECK(g.track(g.v1).find(head)->timelineRange.duration().frames() == 100);
    CHECK(g.track(g.v1).clips()[1].start().frames() == 100);
}

TEST_CASE("A title's clip name follows its words until somebody renames it", "[edit][graphic]") {
    Fixture f;
    model::Graphic title;
    title.kind = model::GraphicKind::Text;
    title.text = "Kestrel Bay";
    REQUIRE(f.run(
        edit::makeAddGraphic(f.project, f.on(f.v1), title, time::TimeRange{f.at(0), f.at(50)})));
    const model::ClipId id = f.track(f.v1).clips().front().id;
    CHECK(f.track(f.v1).find(id)->name == "Kestrel Bay");

    // Retyped, and the name follows -- it was still the generated one.
    title.text = "East Harbour";
    REQUIRE(f.run(edit::makeSetGraphic(f.project, f.on(f.v1), id, title)));
    CHECK(f.track(f.v1).find(id)->name == "East Harbour");

    // Renamed by hand, and now it does not: somebody who called a title
    // "lower third" meant it, and having that replaced the next time the words
    // were edited would be the editor undoing their work.
    f.track(f.v1).find(id)->name = "lower third";
    title.text = "Something else";
    REQUIRE(f.run(edit::makeSetGraphic(f.project, f.on(f.v1), id, title)));
    CHECK(f.track(f.v1).find(id)->name == "lower third");

    // A shape's name is what it is, and editing its box leaves it alone.
    model::Graphic shape;
    shape.kind = model::GraphicKind::Rectangle;
    REQUIRE(f.run(
        edit::makeAddGraphic(f.project, f.on(f.v2), shape, time::TimeRange{f.at(0), f.at(50)})));
    const model::ClipId shapeId = f.track(f.v2).clips().front().id;
    CHECK(f.track(f.v2).find(shapeId)->name == "rectangle");
    shape.width = 900.0;
    REQUIRE(f.run(edit::makeSetGraphic(f.project, f.on(f.v2), shapeId, shape)));
    CHECK(f.track(f.v2).find(shapeId)->name == "rectangle");
}
