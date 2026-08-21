#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/io/CubeLut.h"
#include "zaro/core/render/BakeLut.h"
#include "zaro/core/render/ColorPipeline.h"
#include "zaro/core/render/Grade.h"

#include "ModelFixtures.h"

using namespace zaro;
using Catch::Approx;
using zaro::testing::Fixture;

namespace {

model::Clip graded(Fixture& f) {
    model::Clip clip = f.clip(0, 50);
    return clip;
}

}  // namespace

TEST_CASE("A neutral clip bakes to an identity cube", "[render][bakelut]") {
    // The check that says the round trip is sound: nothing set, nothing
    // changed, and the file reads back as the function that does nothing.
    Fixture f;
    render::LutOmissions omissions;
    auto text =
        render::bakeCube(graded(f), media::TransferFunction::BT709, 17, "neutral", &omissions);
    REQUIRE(text);
    CHECK_FALSE(omissions.any());

    auto parsed = io::CubeLut::parse(*text);
    REQUIRE(parsed);
    CHECK(parsed->size() == 17);
    CHECK(parsed->title() == "neutral");

    for (const float value : {0.0F, 0.25F, 0.5F, 0.75F, 1.0F}) {
        float r = value;
        float g = value;
        float b = value;
        parsed->apply(r, g, b);
        INFO("value " << value);
        CHECK(r == Approx(value).margin(0.002F));
        CHECK(g == Approx(value).margin(0.002F));
        CHECK(b == Approx(value).margin(0.002F));
    }
}

TEST_CASE("The baked cube reproduces what the grade does", "[render][bakelut]") {
    // What a look file is for: the same colour in, the same colour out, in
    // somebody else's program.
    Fixture f;
    model::Clip clip = graded(f);
    clip.color.saturation = 40.0;
    clip.color.exposure = 0.4;
    clip.wheels.offsetB = 0.03;
    clip.wheels.powerR = 0.9;

    render::LutOmissions omissions;
    auto text = render::bakeCube(clip, media::TransferFunction::BT709, 33, "look", &omissions);
    REQUIRE(text);
    auto parsed = io::CubeLut::parse(*text);
    REQUIRE(parsed);

    const render::GradeConstants grade = render::gradeConstantsFor(clip.color, clip.wheels);
    for (const auto& sample :
         {std::array<float, 3>{0.2F, 0.5F, 0.8F}, std::array<float, 3>{0.9F, 0.3F, 0.1F},
          std::array<float, 3>{0.5F, 0.5F, 0.5F}}) {
        // Through the grade, the long way.
        float r = render::toLinearScalar(sample[0], media::TransferFunction::BT709);
        float g = render::toLinearScalar(sample[1], media::TransferFunction::BT709);
        float b = render::toLinearScalar(sample[2], media::TransferFunction::BT709);
        render::gradePixel(grade, r, g, b, nullptr, nullptr, nullptr, 1.0F);
        const float wantR = render::fromLinearScalar(r, media::TransferFunction::BT709);
        const float wantG = render::fromLinearScalar(g, media::TransferFunction::BT709);
        const float wantB = render::fromLinearScalar(b, media::TransferFunction::BT709);

        // And through the file.
        float lr = sample[0];
        float lg = sample[1];
        float lb = sample[2];
        parsed->apply(lr, lg, lb);

        INFO("sample " << sample[0] << "," << sample[1] << "," << sample[2]);
        // A 33-cube interpolates, so this is the cube's own resolution rather
        // than a disagreement about the grade.
        CHECK(lr == Approx(wantR).margin(0.01F));
        CHECK(lg == Approx(wantG).margin(0.01F));
        CHECK(lb == Approx(wantB).margin(0.01F));
    }
}

TEST_CASE("What a look file cannot carry is said, not dropped", "[render][bakelut]") {
    // The one thing a look file must not do is fail to match the shot it came
    // from without saying so.
    Fixture f;
    model::Clip clip = graded(f);
    clip.mask.shape = model::MaskShape::Ellipse;
    clip.vignette.amount = -0.5;
    clip.keyer.kind = model::KeyKind::Chroma;
    model::Effect blur;
    blur.kind = model::EffectKind::Blur;
    blur.setValue(model::EffectParam::Radius, 4.0);
    clip.effects.push_back(blur);

    render::LutOmissions omissions;
    REQUIRE(render::bakeCube(clip, media::TransferFunction::BT709, 9, "", &omissions));
    CHECK(omissions.mask);
    CHECK(omissions.vignette);
    CHECK(omissions.keyer);
    CHECK(omissions.effects);
    const std::string said = omissions.describe();
    INFO(said);
    CHECK(said.find("mask") != std::string::npos);
    CHECK(said.find("vignette") != std::string::npos);
    CHECK(said.find("effects") != std::string::npos);
}

TEST_CASE("A grade that lifts past white says so too", "[render][bakelut]") {
    // A 0..1 cube has nowhere to put it, and the difference between the file
    // and the shot is exactly what somebody needs to know before shipping it.
    Fixture f;
    model::Clip clip = graded(f);
    clip.color.exposure = 2.0;

    render::LutOmissions omissions;
    REQUIRE(render::bakeCube(clip, media::TransferFunction::BT709, 9, "", &omissions));
    CHECK(omissions.aboveWhite);

    SECTION("and a grade that stays inside it does not") {
        model::Clip gentle = graded(f);
        gentle.color.exposure = -1.0;
        render::LutOmissions quiet;
        REQUIRE(render::bakeCube(gentle, media::TransferFunction::BT709, 9, "", &quiet));
        CHECK_FALSE(quiet.aboveWhite);
        CHECK_FALSE(quiet.any());
    }
}

TEST_CASE("A cube of an unusable size is refused", "[render][bakelut]") {
    Fixture f;
    CHECK_FALSE(render::bakeCube(graded(f), media::TransferFunction::BT709, 1, "", nullptr));
    CHECK_FALSE(render::bakeCube(graded(f), media::TransferFunction::BT709, 500, "", nullptr));
}

TEST_CASE("The cube is written in the order the format specifies", "[render][bakelut]") {
    // Red fastest, then green, then blue. Getting this backwards produces a
    // file that loads without complaint and swaps two channels of every look.
    Fixture f;
    model::Clip clip = graded(f);
    // Something that tells the channels apart: only red is touched.
    clip.wheels.slopeR = 0.5;

    auto text = render::bakeCube(clip, media::TransferFunction::BT709, 2, "", nullptr);
    REQUIRE(text);
    auto parsed = io::CubeLut::parse(*text);
    REQUIRE(parsed);

    float r = 1.0F;
    float g = 1.0F;
    float b = 1.0F;
    parsed->apply(r, g, b);
    CHECK(r < 0.9F);
    CHECK(g == Approx(1.0F).margin(0.01F));
    CHECK(b == Approx(1.0F).margin(0.01F));
}
