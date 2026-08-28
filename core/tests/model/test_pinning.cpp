#include <cstdint>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/core/model/Sequence.h"

#include "ModelFixtures.h"

using namespace zaro;
using Catch::Approx;
using zaro::testing::Fixture;

namespace {

const time::Rational k25 = time::rates::fps25;

time::RationalTime at(std::int64_t frames) {
    return time::RationalTime{frames, k25};
}

/// A shot on the lower track and a title over it, both a second long.
struct Pinned {
    Fixture fixture;
    model::SequenceId sequenceId;
    model::TrackId lower;
    model::TrackId upper;
    model::ClipId shot;
    model::ClipId title;
    edit::CommandStack commands;

    Pinned() {
        sequenceId = fixture.sequenceId;
        lower = fixture.v1;
        upper = fixture.v2;

        model::Clip shotClip;
        shotClip.id = fixture.project.ids().next<model::ClipTag>();
        shotClip.source = fixture.longMedia;
        shotClip.timelineRange = time::TimeRange{at(0), at(24)};
        shotClip.sourceRange = time::TimeRange{at(0), at(24)};
        shot = shotClip.id;
        auto placedShot = edit::makeOverwrite(fixture.project, {sequenceId, lower}, shotClip);
        REQUIRE(placedShot);
        commands.execute(fixture.project, std::move(*placedShot));

        model::Clip titleClip;
        titleClip.id = fixture.project.ids().next<model::ClipTag>();
        titleClip.graphic.kind = model::GraphicKind::Text;
        titleClip.graphic.text = "over it";
        titleClip.timelineRange = time::TimeRange{at(0), at(24)};
        titleClip.sourceRange = time::TimeRange{at(0), at(24)};
        titleClip.transform.positionX = 100.0;
        titleClip.transform.positionY = 50.0;
        title = titleClip.id;
        auto placedTitle = edit::makeOverwrite(fixture.project, {sequenceId, upper}, titleClip);
        REQUIRE(placedTitle);
        commands.execute(fixture.project, std::move(*placedTitle));
    }

    void moveShot(double x, double y, double scale = 1.0) {
        model::Clip* clip = fixture.project.findSequence(sequenceId)->findTrack(lower)->find(shot);
        clip->transform.positionX = x;
        clip->transform.positionY = y;
        clip->transform.scaleX = scale;
        clip->transform.scaleY = scale;
    }

    [[nodiscard]] model::Transform titleTransform(std::int64_t frame = 0) const {
        const model::Sequence& sequence = *fixture.project.findSequence(sequenceId);
        return model::pinnedTransformAt(sequence, *sequence.findTrack(upper)->find(title),
                                        at(frame));
    }
};

}  // namespace

TEST_CASE("an unpinned title stays where it was put", "[pinning]") {
    Pinned world;
    world.moveShot(200.0, -80.0, 2.0);

    const model::Transform transform = world.titleTransform();
    CHECK(transform.positionX == Approx(100.0));
    CHECK(transform.positionY == Approx(50.0));
    CHECK(transform.scaleX == Approx(1.0));
}

TEST_CASE("a pinned title follows the shot's position and scale", "[pinning]") {
    Pinned world;
    auto pinned = edit::makePinTo(world.fixture.project, {world.sequenceId, world.upper},
                                  world.title, world.shot);
    REQUIRE(pinned);
    world.commands.execute(world.fixture.project, std::move(*pinned));
    world.moveShot(200.0, -80.0, 2.0);

    const model::Transform transform = world.titleTransform();
    // Its own offset is measured in the shot's frame, so it doubles with it.
    CHECK(transform.positionX == Approx(200.0 + 200.0));
    CHECK(transform.positionY == Approx(-80.0 + 100.0));
    CHECK(transform.scaleX == Approx(2.0));
    CHECK(transform.scaleY == Approx(2.0));
}

TEST_CASE("a pinned title turns with the shot", "[pinning]") {
    Pinned world;
    auto pinned = edit::makePinTo(world.fixture.project, {world.sequenceId, world.upper},
                                  world.title, world.shot);
    REQUIRE(pinned);
    world.commands.execute(world.fixture.project, std::move(*pinned));
    model::Clip* clip = world.fixture.project.findSequence(world.sequenceId)
                            ->findTrack(world.lower)
                            ->find(world.shot);
    clip->transform.rotationDegrees = 90.0;

    const model::Transform transform = world.titleTransform();
    // A quarter turn takes (100, 50) to (-50, 100).
    CHECK(transform.positionX == Approx(-50.0).margin(0.001));
    CHECK(transform.positionY == Approx(100.0).margin(0.001));
    CHECK(transform.rotationDegrees == Approx(90.0));
}

TEST_CASE("opacity is not inherited through a pin", "[pinning]") {
    Pinned world;
    auto pinned = edit::makePinTo(world.fixture.project, {world.sequenceId, world.upper},
                                  world.title, world.shot);
    REQUIRE(pinned);
    world.commands.execute(world.fixture.project, std::move(*pinned));
    model::Clip* clip = world.fixture.project.findSequence(world.sequenceId)
                            ->findTrack(world.lower)
                            ->find(world.shot);
    clip->transform.opacity = 0.0;

    // A title over a dissolve is usually meant to survive it.
    CHECK(world.titleTransform().opacity == Approx(1.0));
}

TEST_CASE("a pin only applies where the host is", "[pinning]") {
    Pinned world;
    auto pinned = edit::makePinTo(world.fixture.project, {world.sequenceId, world.upper},
                                  world.title, world.shot);
    REQUIRE(pinned);
    world.commands.execute(world.fixture.project, std::move(*pinned));
    world.moveShot(200.0, 0.0);

    // Shorten the shot to half the title's length: past its end there is no
    // transform to follow, and extrapolating one would put the title somewhere
    // nothing on the timeline explains.
    model::Clip* clip = world.fixture.project.findSequence(world.sequenceId)
                            ->findTrack(world.lower)
                            ->find(world.shot);
    clip->timelineRange = time::TimeRange{at(0), at(12)};
    clip->sourceRange = time::TimeRange{at(0), at(12)};

    CHECK(world.titleTransform(6).positionX == Approx(300.0));
    CHECK(world.titleTransform(18).positionX == Approx(100.0));
}

TEST_CASE("a pin cannot be made into a loop", "[pinning]") {
    Pinned world;
    auto first = edit::makePinTo(world.fixture.project, {world.sequenceId, world.upper},
                                 world.title, world.shot);
    REQUIRE(first);
    world.commands.execute(world.fixture.project, std::move(*first));

    auto backwards = edit::makePinTo(world.fixture.project, {world.sequenceId, world.lower},
                                     world.shot, world.title);
    CHECK_FALSE(backwards);

    auto itself = edit::makePinTo(world.fixture.project, {world.sequenceId, world.upper},
                                  world.title, world.title);
    CHECK_FALSE(itself);
}
