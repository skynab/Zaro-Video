#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/edit/Operations.h"

#include "ModelFixtures.h"

using namespace zaro;
using zaro::testing::Fixture;

namespace {

const time::Rational k25 = time::rates::fps25;

time::RationalTime at(std::int64_t frames) {
    return time::RationalTime{frames, k25};
}

time::TimeRange span(std::int64_t from, std::int64_t frames) {
    return time::TimeRange{at(from), at(frames)};
}

/// A hundred frames of picture on V1 and the same of sound on A1, with a
/// transcript over it: what a text-based edit is applied to.
struct Talking {
    Fixture fixture;
    edit::CommandStack commands;

    Talking() {
        model::Clip picture;
        picture.id = fixture.project.ids().next<model::ClipTag>();
        picture.source = fixture.longMedia;
        picture.timelineRange = span(0, 100);
        picture.sourceRange = span(0, 100);
        auto placed =
            edit::makeOverwrite(fixture.project, {fixture.sequenceId, fixture.v1}, picture);
        REQUIRE(placed);
        commands.execute(fixture.project, std::move(*placed));

        model::Clip sound;
        sound.id = fixture.project.ids().next<model::ClipTag>();
        sound.source = fixture.longMedia;
        sound.timelineRange = span(0, 100);
        sound.sourceRange = span(0, 100);
        auto laid = edit::makeOverwrite(fixture.project, {fixture.sequenceId, fixture.a1}, sound);
        REQUIRE(laid);
        commands.execute(fixture.project, std::move(*laid));

        model::CaptionTrack transcript;
        for (const auto& [from, frames, text] :
             {std::tuple{0, 20, "the first thing"}, std::tuple{20, 20, "the um second thing"},
              std::tuple{40, 20, "the third thing"}, std::tuple{60, 40, "and the last thing"}}) {
            model::Caption line;
            line.range = span(from, frames);
            line.text = text;
            transcript.add(line);
        }
        auto said = edit::makeSetCaptions(fixture.project, fixture.sequenceId, transcript);
        REQUIRE(said);
        commands.execute(fixture.project, std::move(*said));
    }

    [[nodiscard]] model::Sequence& sequence() {
        return *fixture.project.findSequence(fixture.sequenceId);
    }
    [[nodiscard]] time::RationalTime lengthOf(model::TrackId track) {
        const model::Track* found = sequence().findTrack(track);
        return found->clips().empty() ? at(0) : found->clips().back().endExclusive();
    }
};

}  // namespace

TEST_CASE("deleting a span shortens every track by it", "[spans]") {
    Talking world;
    auto built =
        edit::makeDeleteSpans(world.fixture.project, world.fixture.sequenceId, {span(20, 20)});
    REQUIRE(built);
    world.commands.execute(world.fixture.project, std::move(*built));

    CHECK(world.lengthOf(world.fixture.v1) == at(80));
    // Sound and picture stay the same length, which is what keeps them in sync.
    CHECK(world.lengthOf(world.fixture.a1) == at(80));
}

TEST_CASE("the transcript comes with it", "[spans]") {
    Talking world;
    auto built =
        edit::makeDeleteSpans(world.fixture.project, world.fixture.sequenceId, {span(20, 20)});
    REQUIRE(built);
    world.commands.execute(world.fixture.project, std::move(*built));

    const auto& lines = world.sequence().captions().captions();
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].text == "the first thing");
    // The line that was deleted has gone, and the ones after it moved earlier
    // by exactly what was removed.
    CHECK(lines[1].text == "the third thing");
    CHECK(lines[1].range.start() == at(20));
    CHECK(lines[2].text == "and the last thing");
    CHECK(lines[2].range.start() == at(40));
}

TEST_CASE("several spans go at once, and the later ones land where they should", "[spans]") {
    Talking world;
    // Two lines, not next to each other: the second is only correct if the
    // removals happen latest-first.
    auto built = edit::makeDeleteSpans(world.fixture.project, world.fixture.sequenceId,
                                       {span(0, 20), span(40, 20)});
    REQUIRE(built);
    world.commands.execute(world.fixture.project, std::move(*built));

    CHECK(world.lengthOf(world.fixture.v1) == at(60));
    const auto& lines = world.sequence().captions().captions();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].text == "the um second thing");
    CHECK(lines[0].range.start() == at(0));
    CHECK(lines[1].text == "and the last thing");
    CHECK(lines[1].range.start() == at(20));
}

TEST_CASE("spans that touch are one removal", "[spans]") {
    Talking world;
    auto built = edit::makeDeleteSpans(world.fixture.project, world.fixture.sequenceId,
                                       {span(20, 20), span(30, 20)});
    REQUIRE(built);
    world.commands.execute(world.fixture.project, std::move(*built));

    // 20..50 removed once, not 20..40 and 30..50 taking the overlap twice.
    CHECK(world.lengthOf(world.fixture.v1) == at(70));
}

TEST_CASE("a caption straddling the cut keeps what was outside it", "[spans]") {
    Talking world;
    // Half of the last line, which runs 60..100.
    auto built =
        edit::makeDeleteSpans(world.fixture.project, world.fixture.sequenceId, {span(70, 10)});
    REQUIRE(built);
    world.commands.execute(world.fixture.project, std::move(*built));

    const auto& lines = world.sequence().captions().captions();
    REQUIRE(lines.size() == 4);
    // The words either side of a cut are still the words either side of it:
    // dropping the whole line because one word went would remove speech nobody
    // asked to remove.
    CHECK(lines.back().text == "and the last thing");
    CHECK(lines.back().range.start() == at(60));
    CHECK(lines.back().range.duration() == at(30));
}

TEST_CASE("deleting nothing is refused", "[spans]") {
    Talking world;
    CHECK_FALSE(edit::makeDeleteSpans(world.fixture.project, world.fixture.sequenceId, {}));
    CHECK_FALSE(
        edit::makeDeleteSpans(world.fixture.project, world.fixture.sequenceId, {span(10, 0)}));
}

TEST_CASE("it undoes as one step", "[spans]") {
    Talking world;
    auto built = edit::makeDeleteSpans(world.fixture.project, world.fixture.sequenceId,
                                       {span(0, 20), span(40, 20)});
    REQUIRE(built);
    world.commands.execute(world.fixture.project, std::move(*built));
    REQUIRE(world.lengthOf(world.fixture.v1) == at(60));

    world.commands.undo(world.fixture.project);
    CHECK(world.lengthOf(world.fixture.v1) == at(100));
    CHECK(world.sequence().captions().size() == 4);
}
