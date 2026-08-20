#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"

#include "ModelFixtures.h"

using namespace zaro;
using Catch::Approx;
using zaro::testing::Fixture;

namespace {

/// A sequence with one track and one clip on it, so it has a duration.
model::SequenceId addSequence(Fixture& f, const std::string& name) {
    model::Sequence sequence{f.project.ids().next<model::SequenceTag>(), name, time::rates::fps25};
    sequence.setSize(1920, 1080);
    const model::SequenceId id = sequence.id();
    const auto track = f.project.ids().next<model::TrackTag>();
    sequence.addTrack(track, model::TrackKind::Video, "V1");
    f.project.addSequence(std::move(sequence));
    REQUIRE(f.run(edit::makeOverwrite(f.project, {id, track}, f.clip(0, 50, 500))));
    return id;
}

}  // namespace

TEST_CASE("A sequence cannot contain itself", "[edit][nest]") {
    Fixture f;
    CHECK(f.project.nestingWouldCycle(f.sequenceId, f.sequenceId));
    CHECK_FALSE(edit::makeNestSequence(f.project, f.on(f.v1), f.sequenceId, f.at(0)));
}

TEST_CASE("A cycle through a chain of nests is refused too", "[edit][nest]") {
    // The whole reason this is a walk rather than one comparison: A holding B
    // holding C is fine until somebody puts A inside C.
    Fixture f;
    const model::SequenceId b = addSequence(f, "B");
    const model::SequenceId c = addSequence(f, "C");

    const auto trackOf = [&](model::SequenceId id) {
        return f.project.findSequence(id)->videoTracks().front().id();
    };

    // A contains B, B contains C.
    REQUIRE(f.run(edit::makeNestSequence(f.project, f.on(f.v1), b, f.at(100))));
    REQUIRE(f.run(edit::makeNestSequence(f.project, {b, trackOf(b)}, c, f.at(100))));

    // Now C cannot contain A.
    CHECK(f.project.nestingWouldCycle(c, f.sequenceId));
    CHECK_FALSE(edit::makeNestSequence(f.project, {c, trackOf(c)}, f.sequenceId, f.at(0)));

    // And B still cannot contain A either.
    CHECK_FALSE(edit::makeNestSequence(f.project, {b, trackOf(b)}, f.sequenceId, f.at(200)));
}

TEST_CASE("The same sequence twice is not a cycle", "[edit][nest]") {
    // A diamond is a perfectly ordinary thing to build: the same graphic used
    // in two places. Only a loop is a problem.
    Fixture f;
    const model::SequenceId inner = addSequence(f, "inner");
    REQUIRE(f.run(edit::makeNestSequence(f.project, f.on(f.v1), inner, f.at(0))));
    REQUIRE(f.run(edit::makeNestSequence(f.project, f.on(f.v2), inner, f.at(0))));
    CHECK_FALSE(f.project.nestingWouldCycle(f.sequenceId, inner));
}

TEST_CASE("A nested clip covers the whole of what it nests", "[edit][nest]") {
    Fixture f;
    const model::SequenceId inner = addSequence(f, "inner");
    REQUIRE(f.run(edit::makeNestSequence(f.project, f.on(f.v1), inner, f.at(30))));

    const model::Track& track = f.track(f.v1);
    REQUIRE(track.clips().size() == 1);
    const model::Clip& clip = track.clips().front();
    CHECK(clip.nested == inner);
    CHECK(clip.start().frames() == 30);
    // The inner sequence is fifty frames long, so the clip is too.
    CHECK(clip.duration().frames() == 50);
    CHECK(clip.sourceRange.start().frames() == 0);
    CHECK(clip.name == "inner");
}

TEST_CASE("An empty sequence is not worth nesting", "[edit][nest]") {
    // A clip of no length is not something anyone meant to make, and it would
    // sit on the timeline being impossible to select.
    Fixture f;
    model::Sequence empty{f.project.ids().next<model::SequenceTag>(), "empty", time::rates::fps25};
    const model::SequenceId id = empty.id();
    f.project.addSequence(std::move(empty));
    CHECK_FALSE(edit::makeNestSequence(f.project, f.on(f.v1), id, f.at(0)));
}
