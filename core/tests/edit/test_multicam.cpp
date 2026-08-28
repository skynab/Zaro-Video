#include <cstdint>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/render/RenderGraph.h"

#include "ModelFixtures.h"
#include "TestSources.h"

using namespace zaro;
using Catch::Approx;
using zaro::testing::Fixture;
using zaro::testing::SolidFrameSource;

namespace {

std::vector<model::Clip::Angle> twoAngles(Fixture& f, model::MediaRefId a, model::MediaRefId b,
                                          std::int64_t offsetB) {
    model::Clip::Angle first;
    first.media = a;
    first.offset = f.at(0);
    first.name = "A";

    model::Clip::Angle second;
    second.media = b;
    // Angles rarely start rolling together, so the second is offset.
    second.offset = f.at(offsetB);
    second.name = "B";
    return {first, second};
}

}  // namespace

TEST_CASE("A multicam clip reads the angle that is live", "[edit][multicam]") {
    Fixture f;
    const model::MediaRefId other = f.addMedia("second.mov", 10000);
    REQUIRE(f.run(edit::makeMulticam(f.project, f.on(f.v1), twoAngles(f, f.longMedia, other, 0),
                                     f.range(0, 50))));

    const model::Clip& clip = f.track(f.v1).clips().front();
    CHECK(clip.isMulticam());
    CHECK(clip.activeSource() == f.longMedia);

    // And an out-of-range angle falls back to the first rather than reading
    // past the end of the list.
    model::Clip broken = clip;
    broken.activeAngle = 7;
    CHECK(broken.activeSource() == f.longMedia);
    broken.activeAngle = -3;
    CHECK(broken.activeSource() == f.longMedia);
}

TEST_CASE("An angle's offset is what syncs it", "[edit][multicam]") {
    // Storing an offset rather than trimming each angle to a common start means
    // switching never re-derives the sync -- and a switch that lands a frame
    // out only sometimes is unfindable.
    Fixture f;
    const model::MediaRefId other = f.addMedia("second.mov", 10000);
    REQUIRE(f.run(edit::makeMulticam(f.project, f.on(f.v1), twoAngles(f, f.longMedia, other, 120),
                                     f.range(0, 50))));

    model::Clip clip = f.track(f.v1).clips().front();
    // Ten frames in, angle A is at its own frame ten.
    CHECK(clip.activeSourceTimeAt(f.at(10)).frames() == 10);

    clip.activeAngle = 1;
    // The same moment on angle B is a hundred and twenty frames later in its
    // own material.
    CHECK(clip.activeSourceTimeAt(f.at(10)).frames() == 130);
}

TEST_CASE("Switching an angle is a cut", "[edit][multicam]") {
    // Modelling it as anything else would mean a second kind of edit that
    // trims, transitions and ripples all had to learn about.
    Fixture f;
    const model::MediaRefId other = f.addMedia("second.mov", 10000);
    REQUIRE(f.run(edit::makeMulticam(f.project, f.on(f.v1), twoAngles(f, f.longMedia, other, 120),
                                     f.range(0, 50))));
    const model::ClipId id = f.track(f.v1).clips().front().id;

    REQUIRE(f.run(edit::makeSwitchAngle(f.project, f.on(f.v1), id, 1, f.at(20))));

    const model::Track& track = f.track(f.v1);
    REQUIRE(track.clips().size() == 2);
    CHECK(track.clips()[0].activeAngle == 0);
    CHECK(track.clips()[0].duration().frames() == 20);
    CHECK(track.clips()[1].activeAngle == 1);
    CHECK(track.clips()[1].start().frames() == 20);
    CHECK(track.clips()[1].duration().frames() == 30);

    // The cut is seamless in source terms: the second piece picks up exactly
    // where the first left off.
    CHECK(track.clips()[1].sourceRange.start() == track.clips()[0].sourceRange.endExclusive());
}

TEST_CASE("Switching at the first frame changes the clip rather than splitting it",
          "[edit][multicam]") {
    // A split there would leave a piece of no length, which is not something
    // anyone meant to make.
    Fixture f;
    const model::MediaRefId other = f.addMedia("second.mov", 10000);
    REQUIRE(f.run(edit::makeMulticam(f.project, f.on(f.v1), twoAngles(f, f.longMedia, other, 0),
                                     f.range(0, 50))));
    const model::ClipId id = f.track(f.v1).clips().front().id;

    REQUIRE(f.run(edit::makeSwitchAngle(f.project, f.on(f.v1), id, 1, f.at(0))));
    REQUIRE(f.track(f.v1).clips().size() == 1);
    CHECK(f.track(f.v1).clips().front().activeAngle == 1);
    CHECK(f.track(f.v1).clips().front().duration().frames() == 50);
}

TEST_CASE("A switch that means nothing is refused", "[edit][multicam]") {
    Fixture f;
    const model::MediaRefId other = f.addMedia("second.mov", 10000);
    REQUIRE(f.run(edit::makeMulticam(f.project, f.on(f.v1), twoAngles(f, f.longMedia, other, 0),
                                     f.range(0, 50))));
    const model::ClipId id = f.track(f.v1).clips().front().id;

    CHECK_FALSE(edit::makeSwitchAngle(f.project, f.on(f.v1), id, 0, f.at(20)));  // already live
    CHECK_FALSE(edit::makeSwitchAngle(f.project, f.on(f.v1), id, 5, f.at(20)));  // no such angle
    CHECK_FALSE(edit::makeSwitchAngle(f.project, f.on(f.v1), id, 1, f.at(90)));  // outside it

    // And an ordinary clip has no angles to switch between.
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v2), f.clip(0, 20, 500))));
    const model::ClipId plain = f.track(f.v2).clips().front().id;
    CHECK_FALSE(edit::makeSwitchAngle(f.project, f.on(f.v2), plain, 1, f.at(10)));
}

TEST_CASE("A multicam clip needs angles that exist", "[edit][multicam]") {
    Fixture f;
    CHECK_FALSE(edit::makeMulticam(f.project, f.on(f.v1), {}, f.range(0, 50)));

    model::Clip::Angle missing;
    missing.media = model::MediaRefId{9999};
    CHECK_FALSE(edit::makeMulticam(f.project, f.on(f.v1), {missing}, f.range(0, 50)));
}

TEST_CASE("The live angle is what composites", "[edit][multicam]") {
    Fixture f;
    f.sequence().setSize(16, 16);
    const model::MediaRefId other = f.addMedia("second.mov", 10000);
    SolidFrameSource source{16, 16};
    source.define(f.longMedia, render::Rgba{1.0F, 0.0F, 0.0F, 1.0F});
    source.define(other, render::Rgba{0.0F, 0.0F, 1.0F, 1.0F});
    render::RenderGraph graph{source};

    REQUIRE(f.run(edit::makeMulticam(f.project, f.on(f.v1), twoAngles(f, f.longMedia, other, 0),
                                     f.range(0, 50))));
    const model::ClipId id = f.track(f.v1).clips().front().id;
    CHECK(graph.composite(f.sequence(), f.at(10))->at(8, 8).r == Approx(1.0F));

    REQUIRE(f.run(edit::makeSwitchAngle(f.project, f.on(f.v1), id, 1, f.at(20))));
    // Before the switch, still the first angle; after it, the second.
    CHECK(graph.composite(f.sequence(), f.at(10))->at(8, 8).r == Approx(1.0F));
    CHECK(graph.composite(f.sequence(), f.at(30))->at(8, 8).b == Approx(1.0F));
}
