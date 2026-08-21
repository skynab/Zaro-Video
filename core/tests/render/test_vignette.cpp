#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"
#include "zaro/core/render/RenderGraph.h"
#include "zaro/core/render/ShapeRaster.h"

#include "ModelFixtures.h"
#include "TestSources.h"

using namespace zaro;
using Catch::Approx;
using zaro::testing::Fixture;
using zaro::testing::SolidFrameSource;

TEST_CASE("A vignette leaves the centre alone and moves the corners", "[render][vignette]") {
    model::Vignette vignette;
    vignette.amount = -0.5;
    vignette.midpoint = 0.4;
    vignette.feather = 0.6;

    // The centre of a 64x64 frame.
    CHECK(render::vignetteGain(vignette, 64, 64, 32, 32) == Approx(1.0F).margin(0.02F));
    // And a corner, which is furthest away.
    CHECK(render::vignetteGain(vignette, 64, 64, 0, 0) < 0.6F);
    CHECK(render::vignetteGain(vignette, 64, 64, 63, 63) < 0.6F);
}

TEST_CASE("A vignette of nothing is exactly nothing", "[render][vignette]") {
    // Off costs one comparison, and a frame nobody has vignetted must come out
    // bit for bit unchanged.
    const model::Vignette none;
    CHECK_FALSE(none.isSet());
    for (const std::int32_t at : {0, 15, 31, 63}) {
        CHECK(render::vignetteGain(none, 64, 64, at, at) == 1.0F);
    }
}

TEST_CASE("Roundness chooses between the frame's shape and a circle", "[render][vignette]") {
    model::Vignette oval;
    oval.amount = -1.0;
    oval.midpoint = 0.5;
    oval.feather = 0.5;
    model::Vignette circle = oval;
    circle.roundness = 0.0;

    // On a wide frame, the middle of the short edge is much closer to the
    // centre in pixels than in frame proportions. The two settings therefore
    // disagree there, which is the whole point of the control.
    const float ovalGain = render::vignetteGain(oval, 128, 32, 64, 0);
    const float circleGain = render::vignetteGain(circle, 128, 32, 64, 0);
    CHECK(ovalGain < circleGain);
}

TEST_CASE("A vignette darkens rather than making holes", "[render][vignette]") {
    // The difference from a mask: what is underneath must not show through the
    // corners.
    Fixture f;
    f.sequence().setSize(32, 32);
    SolidFrameSource source{32, 32};
    source.define(f.longMedia, render::Rgba{0.0F, 0.0F, 1.0F, 1.0F});
    source.define(f.shortMedia, render::Rgba{1.0F, 0.0F, 0.0F, 1.0F});
    render::RenderGraph graph{source};

    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 0, f.shortMedia))));
    model::Clip top = f.clip(0, 50);
    const model::ClipId topId = top.id;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v2), top)));

    model::Vignette vignette;
    vignette.amount = -1.0;
    vignette.midpoint = 0.2;
    vignette.feather = 0.4;
    REQUIRE(f.run(edit::makeSetVignette(f.project, f.on(f.v2), topId, vignette)));

    auto frame = graph.composite(f.sequence(), f.at(10));
    REQUIRE(frame);
    const render::Rgba corner = frame->at(0, 0);
    // Dark, opaque, and with none of the red from underneath.
    CHECK(corner.a == Approx(1.0F));
    CHECK(corner.b < 0.3F);
    CHECK(corner.r == Approx(0.0F).margin(0.001F));
    // The middle is untouched.
    CHECK(frame->at(16, 16).b == Approx(1.0F).margin(0.05F));
}

TEST_CASE("A vignette survives a round trip", "[render][vignette][io]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;

    model::Vignette vignette;
    vignette.amount = -0.4;
    vignette.midpoint = 0.55;
    vignette.roundness = 0.25;
    REQUIRE(f.run(edit::makeSetVignette(f.project, f.on(f.v1), clipId, vignette)));

    auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    auto reloaded = io::loadProjectFromString(*text);
    REQUIRE(reloaded);
    const model::Vignette& back =
        reloaded->project.findSequence(f.sequenceId)->findTrack(f.v1)->clips().front().vignette;
    CHECK(back.amount == Approx(-0.4));
    CHECK(back.roundness == Approx(0.25));

    SECTION("and a clip without one writes nothing") {
        Fixture plain;
        REQUIRE(
            plain.run(edit::makeOverwrite(plain.project, plain.on(plain.v1), plain.clip(0, 50))));
        auto bare = io::saveProjectToString(plain.project);
        REQUIRE(bare);
        CHECK(bare->find("\"vignette\"") == std::string::npos);
    }
}

TEST_CASE("A vignette that falls off backwards is refused", "[render][vignette]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;
    model::Vignette backwards;
    backwards.amount = -0.5;
    backwards.feather = -1.0;
    CHECK_FALSE(edit::makeSetVignette(f.project, f.on(f.v1), clipId, backwards));
}
