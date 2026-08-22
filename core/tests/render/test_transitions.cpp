#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/render/RenderGraph.h"
#include "zaro/core/render/TransitionShape.h"

#include "ModelFixtures.h"
#include "TestSources.h"

using namespace zaro;
using Catch::Approx;
using zaro::testing::Fixture;
using zaro::testing::SolidFrameSource;

namespace {

model::Transition transitionOf(model::TransitionKind kind, model::TransitionDirection direction) {
    model::Transition out;
    out.kind = kind;
    out.direction = direction;
    return out;
}

}  // namespace

TEST_CASE("A dissolve is opacity and nothing else", "[render][transition]") {
    const auto shape = render::transitionShapeFor(
        transitionOf(model::TransitionKind::CrossDissolve, model::TransitionDirection::Right), 0.25,
        1920, 1080);
    CHECK(shape.opacity == Approx(0.25));
    CHECK(shape.offsetX == Approx(0.0));
    CHECK_FALSE(shape.wipe.isSet());
}

TEST_CASE("A wipe uncovers from the side it travels from", "[render][transition]") {
    // Right means the edge travels right, so it uncovers from the left. The
    // same word means the same thing for a slide, which is why they share it.
    const auto shape = render::transitionShapeFor(
        transitionOf(model::TransitionKind::Wipe, model::TransitionDirection::Right), 0.25, 1000,
        500);
    REQUIRE(shape.wipe.isSet());
    CHECK(shape.opacity == Approx(1.0));
    CHECK(shape.wipe.width == Approx(250.0));
    CHECK(shape.wipe.height == Approx(500.0));
    // Centred in the quarter it covers: 125 from the left edge, which is -375
    // from the centre of a 1000-wide frame.
    CHECK(shape.wipe.centreX == Approx(-375.0));

    SECTION("and the other way round from the other side") {
        const auto other = render::transitionShapeFor(
            transitionOf(model::TransitionKind::Wipe, model::TransitionDirection::Left), 0.25, 1000,
            500);
        CHECK(other.wipe.centreX == Approx(375.0));
    }

    SECTION("and vertically for up and down") {
        const auto down = render::transitionShapeFor(
            transitionOf(model::TransitionKind::Wipe, model::TransitionDirection::Down), 0.5, 1000,
            500);
        CHECK(down.wipe.height == Approx(250.0));
        CHECK(down.wipe.centreY == Approx(-125.0));
        CHECK(down.wipe.width == Approx(1000.0));
    }
}

TEST_CASE("A wipe covers everything at the end and nothing at the start", "[render][transition]") {
    const auto transition =
        transitionOf(model::TransitionKind::Wipe, model::TransitionDirection::Right);
    const auto start = render::transitionShapeFor(transition, 0.0, 800, 600);
    CHECK(start.wipe.width == Approx(0.0));
    const auto end = render::transitionShapeFor(transition, 1.0, 800, 600);
    CHECK(end.wipe.width == Approx(800.0));
    CHECK(end.wipe.centreX == Approx(0.0));
}

TEST_CASE("A slide starts off screen and ends home", "[render][transition]") {
    const auto transition =
        transitionOf(model::TransitionKind::Slide, model::TransitionDirection::Right);
    const auto start = render::transitionShapeFor(transition, 0.0, 1000, 500);
    CHECK(start.offsetX == Approx(-1000.0));
    CHECK(start.opacity == Approx(1.0));
    CHECK_FALSE(start.wipe.isSet());

    const auto half = render::transitionShapeFor(transition, 0.5, 1000, 500);
    CHECK(half.offsetX == Approx(-500.0));

    const auto end = render::transitionShapeFor(transition, 1.0, 1000, 500);
    CHECK(end.offsetX == Approx(0.0));

    SECTION("and travels the other way when told to") {
        const auto left = render::transitionShapeFor(
            transitionOf(model::TransitionKind::Slide, model::TransitionDirection::Left), 0.5, 1000,
            500);
        CHECK(left.offsetX == Approx(500.0));
        const auto down = render::transitionShapeFor(
            transitionOf(model::TransitionKind::Slide, model::TransitionDirection::Down), 0.5, 1000,
            500);
        CHECK(down.offsetY == Approx(-250.0));
        CHECK(down.offsetX == Approx(0.0));
    }
}

TEST_CASE("A wipe reaches the picture", "[render][transition]") {
    Fixture f;
    f.sequence().setSize(32, 16);
    SolidFrameSource source{32, 16};
    const model::MediaRefId second = f.addMedia("second.mov", 10000);
    source.define(f.longMedia, render::Rgba{1.0F, 0.0F, 0.0F, 1.0F});
    source.define(second, render::Rgba{0.0F, 0.0F, 1.0F, 1.0F});
    render::RenderGraph graph{source};

    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 500, second))));
    const model::ClipId firstId = f.track(f.v1).clips().front().id;
    const model::ClipId secondId = f.track(f.v1).clips().back().id;
    static_cast<void>(firstId);
    static_cast<void>(secondId);
    REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(50), f.at(20))));

    // Made a wipe the way somebody would: drop a dissolve on the cut, then
    // decide it wants to be something else.
    const model::TransitionId transitionId = f.track(f.v1).transitions().front().id;
    const auto range = f.track(f.v1).transitions().front().range;
    REQUIRE(f.run(edit::makeSetTransitionKind(f.project, f.on(f.v1), transitionId,
                                              model::TransitionKind::Wipe,
                                              model::TransitionDirection::Right)));

    // Half way through: the left half is the incoming shot, the right half the
    // outgoing one, and neither is a blend of the two.
    const time::RationalTime middle =
        range.start() + time::RationalTime{range.duration().frames() / 2, range.start().rate()};
    auto frame = graph.composite(f.sequence(), middle);
    REQUIRE(frame);
    CHECK(frame->at(4, 8).b == Approx(1.0F));
    CHECK(frame->at(4, 8).r == Approx(0.0F).margin(0.01F));
    CHECK(frame->at(28, 8).r == Approx(1.0F));
    CHECK(frame->at(28, 8).b == Approx(0.0F).margin(0.01F));
}

TEST_CASE("A transition kind survives a round trip through its name", "[render][transition]") {
    for (const model::TransitionKind kind :
         {model::TransitionKind::CrossDissolve, model::TransitionKind::Wipe,
          model::TransitionKind::Slide}) {
        CHECK(model::transitionKindFromString(model::toString(kind)) == kind);
    }
    // A name from a later version is a cut somebody can still watch, so it
    // falls back rather than refusing the project.
    CHECK(model::transitionKindFromString("morphCut") == model::TransitionKind::CrossDissolve);

    for (const model::TransitionDirection direction :
         {model::TransitionDirection::Right, model::TransitionDirection::Left,
          model::TransitionDirection::Down, model::TransitionDirection::Up}) {
        model::TransitionDirection back{};
        REQUIRE(model::transitionDirectionFromString(model::toString(direction), back));
        CHECK(back == direction);
    }
}
