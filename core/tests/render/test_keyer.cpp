#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"
#include "zaro/core/render/Keyer.h"
#include "zaro/core/render/RenderGraph.h"

#include "ModelFixtures.h"
#include "TestSources.h"

using namespace zaro;
using Catch::Approx;
using zaro::testing::Fixture;
using zaro::testing::SolidFrameSource;

namespace {

render::KeyerConstants greenKey(double tolerance = 0.12, double softness = 0.06) {
    model::Keyer keyer;
    keyer.kind = model::KeyKind::Chroma;
    keyer.red = 0.0;
    keyer.green = 1.0;
    keyer.blue = 0.0;
    keyer.tolerance = tolerance;
    keyer.softness = softness;
    keyer.spill = 0.0;  // measured separately
    return render::keyerConstantsFor(keyer, media::TransferFunction::BT709);
}

}  // namespace

TEST_CASE("The key colour is dropped and other colours are kept", "[render][keyer]") {
    const render::KeyerConstants keyer = greenKey();
    CHECK(render::keyMatte(keyer, 0.0F, 0.6F, 0.0F) == Approx(0.0));
    CHECK(render::keyMatte(keyer, 0.6F, 0.0F, 0.0F) == Approx(1.0));
    CHECK(render::keyMatte(keyer, 0.2F, 0.2F, 0.7F) == Approx(1.0));
}

TEST_CASE("A shadow on the screen is the same colour as the lit part", "[render][keyer]") {
    // The reason the distance is measured in chromaticity. A screen is never
    // lit evenly, and a key that works at the top and not at the bottom is one
    // nobody can use.
    const render::KeyerConstants keyer = greenKey();
    const float bright = render::keyMatte(keyer, 0.02F, 0.90F, 0.02F);
    const float shadowed = render::keyMatte(keyer, 0.005F, 0.225F, 0.005F);
    CHECK(bright == Approx(0.0));
    CHECK(shadowed == Approx(0.0));
}

TEST_CASE("Black in front of the screen is a subject, not a hole", "[render][keyer]") {
    // Near zero there is no reliable colour to measure; dividing by it would
    // turn sensor noise into a hue and punch holes in the darkest part of the
    // shot.
    const render::KeyerConstants keyer = greenKey();
    CHECK(render::keyMatte(keyer, 0.0F, 0.0F, 0.0F) == Approx(1.0));
    CHECK(render::keyMatte(keyer, 1e-6F, 1e-6F, 1e-6F) == Approx(1.0));
}

TEST_CASE("The edge of the key is soft", "[render][keyer]") {
    const render::KeyerConstants keyer = greenKey(0.05, 0.30);
    // Walking away from pure green, the matte has to come up smoothly rather
    // than switch: a hard threshold makes an edge that looks cut out with
    // scissors.
    float previous = -1.0F;
    bool rose = false;
    for (int step = 0; step <= 10; ++step) {
        const auto mix = static_cast<float>(step) / 10.0F;
        const float matte = render::keyMatte(keyer, mix * 0.6F, 0.6F, mix * 0.6F);
        CHECK(matte >= previous - 1e-5F);
        if (matte > 0.0F && matte < 1.0F) {
            rose = true;
        }
        previous = matte;
    }
    CHECK(rose);
}

TEST_CASE("A luma key drops a range of brightness", "[render][keyer]") {
    model::Keyer keyer;
    keyer.kind = model::KeyKind::Luma;
    keyer.lumaLow = 0.0;
    keyer.lumaHigh = 0.2;
    keyer.lumaSoftness = 0.02;
    const render::KeyerConstants constants =
        render::keyerConstantsFor(keyer, media::TransferFunction::BT709);

    CHECK(render::keyMatte(constants, 0.0F, 0.0F, 0.0F) == Approx(0.0));
    CHECK(render::keyMatte(constants, 0.8F, 0.8F, 0.8F) == Approx(1.0));
}

TEST_CASE("Spill suppression pulls the key colour out of what is left", "[render][keyer]") {
    model::Keyer keyer;
    keyer.kind = model::KeyKind::Chroma;
    keyer.red = 0.0;
    keyer.green = 1.0;
    keyer.blue = 0.0;

    SECTION("fully") {
        keyer.spill = 1.0;
        const auto constants = render::keyerConstantsFor(keyer, media::TransferFunction::BT709);
        float r = 0.4F;
        float g = 0.8F;
        float b = 0.2F;
        render::suppressSpill(constants, r, g, b);
        // Green comes down to the mean of the other two; nothing else moves.
        CHECK(g == Approx(0.3F));
        CHECK(r == Approx(0.4F));
        CHECK(b == Approx(0.2F));
    }

    SECTION("by half") {
        keyer.spill = 0.5;
        const auto constants = render::keyerConstantsFor(keyer, media::TransferFunction::BT709);
        float r = 0.4F;
        float g = 0.8F;
        float b = 0.2F;
        render::suppressSpill(constants, r, g, b);
        CHECK(g == Approx(0.55F));
    }

    SECTION("and never adds any") {
        // A pixel with less green than its neighbours is left alone: taking
        // more out would tint the subject magenta, which is the classic
        // over-suppressed composite.
        keyer.spill = 1.0;
        const auto constants = render::keyerConstantsFor(keyer, media::TransferFunction::BT709);
        float r = 0.6F;
        float g = 0.1F;
        float b = 0.6F;
        render::suppressSpill(constants, r, g, b);
        CHECK(g == Approx(0.1F));
    }

    SECTION("and follows the key colour rather than assuming green") {
        keyer.red = 0.0;
        keyer.green = 0.0;
        keyer.blue = 1.0;
        keyer.spill = 1.0;
        const auto constants = render::keyerConstantsFor(keyer, media::TransferFunction::BT709);
        CHECK(constants.spillChannel == 2);
        float r = 0.4F;
        float g = 0.2F;
        float b = 0.9F;
        render::suppressSpill(constants, r, g, b);
        CHECK(b == Approx(0.3F));
    }
}

TEST_CASE("A key lets the clip underneath show through", "[render][keyer]") {
    Fixture f;
    f.sequence().setSize(8, 8);
    SolidFrameSource source{8, 8};
    const model::MediaRefId screen = f.addMedia("screen.mov", 1000);
    source.define(f.longMedia, render::Rgba{0.0F, 0.0F, 0.8F, 1.0F});  // blue, underneath
    source.define(screen, render::Rgba{0.0F, 0.7F, 0.0F, 1.0F});       // green, on top
    render::RenderGraph graph{source};

    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    model::Clip top = f.clip(0, 50, 500, screen);
    const model::ClipId topId = top.id;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v2), top)));

    auto before = graph.composite(f.sequence(), f.at(10));
    REQUIRE(before);
    CHECK(before->at(4, 4).g == Approx(0.7F));

    model::Keyer keyer;
    keyer.kind = model::KeyKind::Chroma;
    keyer.red = 0.0;
    keyer.green = 1.0;
    keyer.blue = 0.0;
    REQUIRE(f.run(edit::makeSetKeyer(f.project, f.on(f.v2), topId, keyer)));

    auto after = graph.composite(f.sequence(), f.at(10));
    REQUIRE(after);
    // The green is gone and the blue underneath it is what is left.
    CHECK(after->at(4, 4).g == Approx(0.0F).margin(0.001));
    CHECK(after->at(4, 4).b == Approx(0.8F));
    CHECK(after->at(4, 4).a == Approx(1.0F));
}

TEST_CASE("Showing the matte shows the matte, not a hole", "[render][keyer]") {
    Fixture f;
    f.sequence().setSize(8, 8);
    SolidFrameSource source{8, 8};
    source.define(f.longMedia, render::Rgba{0.0F, 0.7F, 0.0F, 1.0F});
    render::RenderGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;

    model::Keyer keyer;
    keyer.kind = model::KeyKind::Chroma;
    keyer.red = 0.0;
    keyer.green = 1.0;
    keyer.blue = 0.0;
    keyer.showMatte = true;
    REQUIRE(f.run(edit::makeSetKeyer(f.project, f.on(f.v1), clipId, keyer)));

    auto frame = graph.composite(f.sequence(), f.at(10));
    REQUIRE(frame);
    const render::Rgba pixel = frame->at(4, 4);
    // Black where the key selects, and opaque: a transparent matte would be
    // invisible against whatever happens to be underneath, which is the one
    // thing the view exists to avoid.
    CHECK(pixel.a == Approx(1.0F));
    CHECK(pixel.r == Approx(0.0F).margin(0.001));
    CHECK(pixel.r == Approx(pixel.g).margin(0.001));
    CHECK(pixel.g == Approx(pixel.b).margin(0.001));
}

TEST_CASE("A key survives a round trip through a project file", "[render][keyer][io]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;

    model::Keyer keyer;
    keyer.kind = model::KeyKind::Luma;
    keyer.lumaLow = 0.05;
    keyer.lumaHigh = 0.35;
    keyer.spill = 0.25;
    keyer.showMatte = true;
    REQUIRE(f.run(edit::makeSetKeyer(f.project, f.on(f.v1), clipId, keyer)));

    auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    auto reloaded = io::loadProjectFromString(*text);
    REQUIRE(reloaded);

    const model::Keyer& back =
        reloaded->project.findSequence(f.sequenceId)->findTrack(f.v1)->clips().front().keyer;
    CHECK(back.kind == model::KeyKind::Luma);
    CHECK(back.lumaLow == Approx(0.05));
    CHECK(back.lumaHigh == Approx(0.35));
    CHECK(back.spill == Approx(0.25));
    // The matte view is how somebody is looking at the picture right now, not
    // something about the cut, so it is deliberately not stored.
    CHECK_FALSE(back.showMatte);
}

TEST_CASE("A key with impossible numbers is refused", "[render][keyer]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;

    model::Keyer backwards;
    backwards.kind = model::KeyKind::Luma;
    backwards.lumaLow = 0.8;
    backwards.lumaHigh = 0.2;
    CHECK_FALSE(edit::makeSetKeyer(f.project, f.on(f.v1), clipId, backwards));
}
