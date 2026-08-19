#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/Grade.h"

using namespace zaro;
using Catch::Approx;

namespace {

struct Colour {
    float r{0.0F};
    float g{0.0F};
    float b{0.0F};
};

Colour graded(const model::ColorCorrection& correction, Colour in) {
    const auto constants = render::gradeConstantsFor(correction);
    render::gradePixel(constants, in.r, in.g, in.b);
    return in;
}

float luma(const Colour& c) {
    return (0.2126F * c.r) + (0.7152F * c.g) + (0.0722F * c.b);
}

}  // namespace

TEST_CASE("A neutral correction changes nothing at all", "[render][grade]") {
    const model::ColorCorrection neutral;
    CHECK(neutral.isIdentity());
    CHECK(render::gradeConstantsFor(neutral).isIdentity());

    const Colour out = graded(neutral, Colour{0.3F, 0.55F, 0.72F});
    CHECK(out.r == Approx(0.3F));
    CHECK(out.g == Approx(0.55F));
    CHECK(out.b == Approx(0.72F));
}

TEST_CASE("A stop of exposure is exactly a doubling", "[render][grade]") {
    // The reason the grade is applied in linear light: a stop is a multiply,
    // and it is the same multiply everywhere on the scale.
    model::ColorCorrection brighter;
    brighter.exposure = 1.0;

    for (const float value : {0.02F, 0.18F, 0.5F, 0.9F}) {
        const Colour out = graded(brighter, Colour{value, value, value});
        CHECK(out.r == Approx(value * 2.0F));
    }

    model::ColorCorrection darker;
    darker.exposure = -2.0;
    CHECK(graded(darker, Colour{0.8F, 0.8F, 0.8F}).r == Approx(0.2F));
}

TEST_CASE("Contrast pivots about middle grey, not about half", "[render][grade]") {
    // 0.5 is the middle of an *encoded* signal. In linear light it is nearly
    // two stops above middle grey, and pivoting there would darken every
    // picture that had contrast added to it.
    model::ColorCorrection punchy;
    punchy.contrast = 50.0;

    const Colour pivot =
        graded(punchy, Colour{render::kMiddleGrey, render::kMiddleGrey, render::kMiddleGrey});
    CHECK(pivot.r == Approx(render::kMiddleGrey).margin(1e-6));

    // Above the pivot goes up, below goes down.
    CHECK(graded(punchy, Colour{0.5F, 0.5F, 0.5F}).r > 0.5F);
    CHECK(graded(punchy, Colour{0.05F, 0.05F, 0.05F}).r < 0.05F);

    model::ColorCorrection flat;
    flat.contrast = -50.0;
    CHECK(graded(flat, Colour{0.5F, 0.5F, 0.5F}).r < 0.5F);
    CHECK(graded(flat, Colour{0.05F, 0.05F, 0.05F}).r > 0.05F);
}

TEST_CASE("The two ends of the contrast control undo each other", "[render][grade]") {
    // +100 doubles the exponent and -100 halves it, so they are inverses. A
    // control whose ends are not symmetrical is one nobody can return to
    // neutral by eye.
    model::ColorCorrection up;
    up.contrast = 100.0;
    model::ColorCorrection down;
    down.contrast = -100.0;

    for (const float value : {0.02F, 0.18F, 0.6F, 1.4F}) {
        const Colour there = graded(up, Colour{value, value, value});
        const Colour back = graded(down, there);
        CHECK(back.r == Approx(value).margin(1e-5));
    }
}

TEST_CASE("Contrast leaves non-positive values alone", "[render][grade]") {
    // A fractional power of a negative number is not a number, and one NaN
    // spreads through every pixel it is ever averaged with.
    model::ColorCorrection punchy;
    punchy.contrast = 40.0;

    const Colour out = graded(punchy, Colour{0.0F, -0.02F, 0.3F});
    CHECK(out.r == 0.0F);
    CHECK(out.g == Approx(-0.02F));
    CHECK(std::isfinite(out.b));
}

TEST_CASE("White balance keeps white as bright as it was", "[render][grade]") {
    // Without normalisation the temperature slider is also an exposure slider,
    // and every correction needs a second one to undo it.
    for (const double temperature : {-100.0, -40.0, 0.0, 40.0, 100.0}) {
        model::ColorCorrection balanced;
        balanced.temperature = temperature;
        const Colour white = graded(balanced, Colour{1.0F, 1.0F, 1.0F});
        CHECK(luma(white) == Approx(1.0F).margin(1e-5));
    }
    for (const double tint : {-100.0, 0.0, 100.0}) {
        model::ColorCorrection balanced;
        balanced.tint = tint;
        CHECK(luma(graded(balanced, Colour{1.0F, 1.0F, 1.0F})) == Approx(1.0F).margin(1e-5));
    }
}

TEST_CASE("Warmer means more red and less blue", "[render][grade]") {
    model::ColorCorrection warm;
    warm.temperature = 60.0;
    const Colour out = graded(warm, Colour{0.5F, 0.5F, 0.5F});
    CHECK(out.r > 0.5F);
    CHECK(out.b < 0.5F);

    model::ColorCorrection cool;
    cool.temperature = -60.0;
    const Colour cooled = graded(cool, Colour{0.5F, 0.5F, 0.5F});
    CHECK(cooled.r < 0.5F);
    CHECK(cooled.b > 0.5F);

    // Tint is the other axis: green and magenta, leaving red and blue alone
    // relative to each other.
    model::ColorCorrection magenta;
    magenta.tint = 60.0;
    const Colour tinted = graded(magenta, Colour{0.5F, 0.5F, 0.5F});
    CHECK(tinted.g < 0.5F);
    CHECK(tinted.r == Approx(tinted.b));
}

TEST_CASE("Desaturating preserves brightness", "[render][grade]") {
    // Mixing toward a fixed grey instead of the pixel's own luma would darken
    // saturated colours and lighten dark ones, so a saturation control would
    // double as an unpredictable exposure control.
    const Colour colours[] = {{0.8F, 0.2F, 0.1F}, {0.1F, 0.6F, 0.3F}, {0.2F, 0.2F, 0.9F}};
    for (const Colour& colour : colours) {
        model::ColorCorrection flat;
        flat.saturation = 0.0;
        const Colour grey = graded(flat, colour);
        CHECK(luma(grey) == Approx(luma(colour)).margin(1e-5));
        // Actually grey, not merely as bright.
        CHECK(grey.r == Approx(grey.g).margin(1e-5));
        CHECK(grey.g == Approx(grey.b).margin(1e-5));

        model::ColorCorrection more;
        more.saturation = 180.0;
        CHECK(luma(graded(more, colour)) == Approx(luma(colour)).margin(1e-5));
    }
}

TEST_CASE("Grading an image is independent of how faded it is", "[render][grade]") {
    // The working space is premultiplied. Grading those values directly would
    // make a clip grade differently in the middle of a dissolve than either
    // side of it.
    model::ColorCorrection punchy;
    punchy.exposure = 0.5;
    punchy.contrast = 30.0;
    punchy.saturation = 140.0;
    const auto constants = render::gradeConstantsFor(punchy);

    render::RgbaImage opaque{4, 4};
    opaque.fill(render::Rgba{0.4F, 0.25F, 0.1F, 1.0F});
    render::RgbaImage faded{4, 4};
    faded.fill(render::Rgba{0.4F * 0.25F, 0.25F * 0.25F, 0.1F * 0.25F, 0.25F});

    render::gradeImage(constants, opaque);
    render::gradeImage(constants, faded);

    const render::Rgba& full = opaque.at(2, 2);
    const render::Rgba& part = faded.at(2, 2);
    CHECK(part.a == Approx(0.25F));
    CHECK(part.r / part.a == Approx(full.r).margin(1e-5));
    CHECK(part.g / part.a == Approx(full.g).margin(1e-5));
    CHECK(part.b / part.a == Approx(full.b).margin(1e-5));
}

TEST_CASE("A fully transparent pixel is left alone", "[render][grade]") {
    model::ColorCorrection bright;
    bright.exposure = 3.0;
    render::RgbaImage empty{2, 2};
    empty.clear();
    render::gradeImage(render::gradeConstantsFor(bright), empty);
    // Dividing by zero alpha would invent colour where there is none.
    CHECK(empty.at(0, 0).r == 0.0F);
    CHECK(empty.at(0, 0).a == 0.0F);
}
