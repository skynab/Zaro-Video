#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"
#include "zaro/core/render/Grade.h"

#include "ModelFixtures.h"

using namespace zaro;
using Catch::Approx;
using zaro::testing::Fixture;

namespace {

/// One colour through the grade, with nothing but the wheels set.
render::Rgba through(const model::ColorWheels& wheels, float r, float g, float b) {
    const render::GradeConstants grade =
        render::gradeConstantsFor(model::ColorCorrection{}, wheels);
    render::gradePixel(grade, r, g, b, nullptr, nullptr, nullptr, 1.0F);
    return render::Rgba{r, g, b, 1.0F};
}

}  // namespace

TEST_CASE("The wheels are an ASC CDL", "[render][wheels]") {
    // out = (in * slope + offset) ^ power, per channel. Worth pinning to the
    // arithmetic rather than to a feeling, because the whole reason for using
    // the CDL is that other programs mean the same thing by it.
    model::ColorWheels wheels;
    wheels.slopeR = 2.0;
    wheels.offsetR = 0.1;
    wheels.powerR = 0.5;

    const render::Rgba out = through(wheels, 0.2F, 0.2F, 0.2F);
    CHECK(out.r == Approx(std::pow((0.2 * 2.0) + 0.1, 0.5)).margin(0.001));
    // And the untouched channels are untouched.
    CHECK(out.g == Approx(0.2F));
    CHECK(out.b == Approx(0.2F));
}

TEST_CASE("Each wheel moves the part of the range it is named for", "[render][wheels]") {
    // What makes three wheels feel like shadows, midtones and highlights even
    // though every one of them touches the whole picture.
    const float shadow = 0.02F;
    const float mid = 0.18F;
    const float highlight = 0.9F;

    SECTION("slope scales, so it moves highlights most and leaves black alone") {
        model::ColorWheels wheels;
        wheels.slopeR = wheels.slopeG = wheels.slopeB = 1.2;
        CHECK(through(wheels, 0.0F, 0.0F, 0.0F).r == Approx(0.0F));
        const float shadowLift = through(wheels, shadow, shadow, shadow).r - shadow;
        const float highlightLift = through(wheels, highlight, highlight, highlight).r - highlight;
        CHECK(highlightLift > shadowLift * 5.0F);
    }

    SECTION("offset adds, so it lifts black off zero") {
        model::ColorWheels wheels;
        wheels.offsetR = wheels.offsetG = wheels.offsetB = 0.05;
        CHECK(through(wheels, 0.0F, 0.0F, 0.0F).r == Approx(0.05F));
        // The same amount everywhere, which is what "offset" means.
        CHECK(through(wheels, highlight, highlight, highlight).r ==
              Approx(highlight + 0.05F).margin(0.001F));
    }

    SECTION("power is a gamma, so it pins both ends and moves the middle") {
        model::ColorWheels wheels;
        wheels.powerR = wheels.powerG = wheels.powerB = 0.8;
        CHECK(through(wheels, 0.0F, 0.0F, 0.0F).r == Approx(0.0F));
        CHECK(through(wheels, 1.0F, 1.0F, 1.0F).r == Approx(1.0F));
        CHECK(through(wheels, mid, mid, mid).r > mid);
    }
}

TEST_CASE("Negative light does not become a NaN", "[render][wheels]") {
    // A fractional power of a negative number has no value, and one NaN spreads
    // through everything it is averaged with -- so a single bad pixel would
    // take a whole blurred region with it.
    model::ColorWheels wheels;
    wheels.offsetR = wheels.offsetG = wheels.offsetB = -0.5;
    wheels.powerR = wheels.powerG = wheels.powerB = 0.5;
    const render::Rgba out = through(wheels, 0.1F, 0.1F, 0.1F);
    CHECK(out.r == Approx(0.0F));
    CHECK(std::isfinite(out.r));
}

TEST_CASE("A power of zero is refused rather than dividing by nothing", "[render][wheels]") {
    model::ColorWheels wheels;
    wheels.powerR = 0.0;
    const render::GradeConstants grade =
        render::gradeConstantsFor(model::ColorCorrection{}, wheels);
    CHECK(grade.power[0] > 0.0F);
    CHECK(std::isfinite(through(wheels, 0.5F, 0.5F, 0.5F).r));
}

TEST_CASE("Neutral wheels cost nothing", "[render][wheels]") {
    // Three pow() calls a pixel is not something to pay for by accident.
    const render::GradeConstants grade =
        render::gradeConstantsFor(model::ColorCorrection{}, model::ColorWheels{});
    CHECK_FALSE(grade.wheels);
    CHECK(grade.isIdentity());

    model::ColorWheels moved;
    moved.slopeG = 1.01;
    CHECK_FALSE(render::gradeConstantsFor(model::ColorCorrection{}, moved).isIdentity());
}

TEST_CASE("The wheels survive a round trip", "[render][wheels][io]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    model::Clip& clip = const_cast<model::Clip&>(f.track(f.v1).clips().front());
    clip.wheels.slopeB = 1.3;
    clip.wheels.offsetR = -0.02;
    clip.wheels.powerG = 0.85;

    auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    auto reloaded = io::loadProjectFromString(*text);
    REQUIRE(reloaded);
    const model::ColorWheels& back =
        reloaded->project.findSequence(f.sequenceId)->findTrack(f.v1)->clips().front().wheels;
    CHECK(back.slopeB == Approx(1.3));
    CHECK(back.offsetR == Approx(-0.02));
    CHECK(back.powerG == Approx(0.85));

    SECTION("and a clip nobody has graded writes nothing") {
        Fixture plain;
        REQUIRE(
            plain.run(edit::makeOverwrite(plain.project, plain.on(plain.v1), plain.clip(0, 50))));
        auto bare = io::saveProjectToString(plain.project);
        REQUIRE(bare);
        CHECK(bare->find("\"wheels\"") == std::string::npos);
    }
}
