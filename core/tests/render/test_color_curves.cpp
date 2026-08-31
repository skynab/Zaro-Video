// Hue curves: the wrap, and the neutral in the middle.

#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/ColorCurveTable.h"

using namespace zaro;
using Catch::Approx;

namespace {

model::ColorCurves flat(double y) {
    model::ColorCurves out;
    for (const double x : {0.0, 0.25, 0.5, 0.75}) {
        out.againstHue.set(model::CurvePoint{x, y});
    }
    return out;
}

}  // namespace

TEST_CASE("A hue curve with no points changes nothing", "[render][hue]") {
    const render::ColorCurveTable table{model::ColorCurves{}};
    CHECK(table.isIdentity());
    for (const float hue : {0.0F, 0.1F, 0.5F, 0.99F}) {
        CHECK(table.saturationAt(hue) == Approx(1.0F));
    }
}

TEST_CASE("Half way up the curve is no change at all", "[render][hue]") {
    // 0.5 is neutral, not 0: the value is a multiplier and the curve has to be
    // able to go both ways, so the middle of the range means leave it alone.
    const render::ColorCurveTable table{flat(0.5)};
    CHECK_FALSE(table.isIdentity());
    for (const float hue : {0.0F, 0.2F, 0.6F, 0.95F}) {
        CHECK(table.saturationAt(hue) == Approx(1.0F).margin(1e-5));
    }
}

TEST_CASE("The ends of the curve are the multipliers they say", "[render][hue]") {
    CHECK(render::ColorCurveTable{flat(1.0)}.saturationAt(0.3F) == Approx(2.0F).margin(1e-5));
    CHECK(render::ColorCurveTable{flat(0.0)}.saturationAt(0.3F) == Approx(0.0F).margin(1e-5));
}

TEST_CASE("A hue curve wraps rather than seaming at red", "[render][hue]") {
    // The property the table exists for. A point just below the top of the
    // circle and one just above the bottom are neighbours, and the curve
    // between them has to run the short way -- across red -- not the long way
    // through every other hue.
    model::ColorCurves curves;
    curves.againstHue.set(model::CurvePoint{0.95, 1.0});
    curves.againstHue.set(model::CurvePoint{0.05, 1.0});
    // Somewhere on the far side, pulled down, so the curve is not simply flat.
    curves.againstHue.set(model::CurvePoint{0.5, 0.0});
    const render::ColorCurveTable table{curves};

    // Red, between the two high points and closer to both than to the low one.
    // Without the wrap this sits on the long flat run between 0.05 and 0.95 and
    // reads low; with it, it is between two highs and reads high.
    CHECK(table.saturationAt(0.0F) > 1.5F);
    // And the far side is still pulled down, so the curve is doing something.
    CHECK(table.saturationAt(0.5F) < 0.5F);
}

TEST_CASE("Reading a hue outside the circle wraps into it", "[render][hue]") {
    // A hue computed from floats can land a hair outside its range, and a read
    // that clamped would flatten the curve at red without anybody seeing why.
    const render::ColorCurveTable table{flat(1.0)};
    CHECK(table.saturationAt(1.0F) == Approx(table.saturationAt(0.0F)).margin(1e-5));
    CHECK(table.saturationAt(1.25F) == Approx(table.saturationAt(0.25F)).margin(1e-5));
    CHECK(table.saturationAt(-0.25F) == Approx(table.saturationAt(0.75F)).margin(1e-5));
}

TEST_CASE("A hue curve never asks for a negative saturation", "[render][hue]") {
    // A negative multiplier pushes a pixel through grey and out the other side,
    // which is a different colour rather than a less saturated one. The
    // interpolation does not overshoot, and the bake clamps regardless.
    model::ColorCurves curves;
    curves.againstHue.set(model::CurvePoint{0.0, 0.0});
    curves.againstHue.set(model::CurvePoint{0.1, 1.0});
    curves.againstHue.set(model::CurvePoint{0.2, 0.0});
    const render::ColorCurveTable table{curves};
    for (int i = 0; i < 256; ++i) {
        CHECK(table.saturationAt(static_cast<float>(i) / 256.0F) >= 0.0F);
    }
}

TEST_CASE("A luma curve scales saturation by brightness", "[render][hue]") {
    // Desaturating the shadows: noise in the blacks is chroma noise, and it is
    // cheaper to take the colour out of it than to denoise it.
    model::ColorCurves curves;
    curves.againstLuma.set(model::CurvePoint{0.0, 0.0});
    curves.againstLuma.set(model::CurvePoint{0.5, 0.5});
    curves.againstLuma.set(model::CurvePoint{1.0, 0.5});
    const render::ColorCurveTable table{curves};

    CHECK_FALSE(table.isIdentity());
    CHECK(table.hasLuma());
    // Its own axis, and the hue curve is untouched by it.
    CHECK_FALSE(table.hasHue());
    CHECK(table.saturationAt(0.3F) == Approx(1.0F));

    // Black is stripped and a bright value is left alone. Indexed as the tone
    // curves are, so 0 is black and the far end is well above white.
    CHECK(table.saturationAtLuma(0.0F) == Approx(0.0F).margin(1e-5));
    CHECK(table.saturationAtLuma(1.0F) > 0.5F);
    // Monotone in between rather than jumping: a saturation that stepped would
    // band across a gradient.
    float previous = -1.0F;
    for (int i = 0; i <= 40; ++i) {
        const float value = table.saturationAtLuma(static_cast<float>(i) / 20.0F);
        CHECK(value >= previous - 1e-5F);
        previous = value;
    }
}

TEST_CASE("The two saturation curves compound", "[render][hue]") {
    // A pixel that is both a blue and a shadow gets both adjustments, which is
    // what a product does. Two sequential lerps toward luma would not compound:
    // the second would work on what the first left.
    model::ColorCurves curves;
    for (const double x : {0.0, 0.25, 0.5, 0.75}) {
        curves.againstHue.set(model::CurvePoint{x, 0.25});
    }
    curves.againstLuma.set(model::CurvePoint{0.0, 0.25});
    curves.againstLuma.set(model::CurvePoint{1.0, 0.25});
    const render::ColorCurveTable table{curves};

    CHECK(table.hasHue());
    CHECK(table.hasLuma());
    // Each is a half, so together they are a quarter.
    CHECK(table.saturationAt(0.3F) == Approx(0.5F).margin(1e-4));
    CHECK(table.saturationAtLuma(0.2F) == Approx(0.5F).margin(1e-4));
}

TEST_CASE("A hue shift moves a hue and leaves the others", "[render][hue]") {
    // 0.5 is neutral here too, but the value is an angle: the ends move a hue a
    // sixth of a turn each way, which reaches the neighbouring primary and no
    // further. A control that could swing red to green would make the useful
    // range a sliver at its centre.
    model::ColorCurves curves;
    for (const double x : {0.0, 0.25, 0.5, 0.75}) {
        curves.hueShift.set(model::CurvePoint{x, 0.5});
    }
    curves.hueShift.set(model::CurvePoint{0.6, 1.0});
    const render::ColorCurveTable table{curves};

    CHECK(table.hasShift());
    CHECK_FALSE(table.hasHue());
    CHECK_FALSE(table.hasLuma());

    // Pushed the full amount at the point that was lifted.
    const float moved = table.shiftedHue(0.6F);
    CHECK(moved == Approx(0.6F + (1.0F / 6.0F)).margin(0.01F));
    // And left alone a quarter turn away, where the curve is still neutral.
    CHECK(table.shiftedHue(0.25F) == Approx(0.25F).margin(0.02F));
}

TEST_CASE("A hue shift wraps past red", "[render][hue]") {
    // Pushing a hue near the top of the circle past 1 has to land near the
    // bottom, not clamp at red -- the axis is a circle in both directions.
    model::ColorCurves curves;
    for (const double x : {0.0, 0.25, 0.5, 0.75}) {
        curves.hueShift.set(model::CurvePoint{x, 0.5});
    }
    curves.hueShift.set(model::CurvePoint{0.95, 1.0});
    const render::ColorCurveTable table{curves};

    const float moved = table.shiftedHue(0.95F);
    CHECK(moved >= 0.0F);
    CHECK(moved < 1.0F);
    // 0.95 + 1/6 is about 1.117, which is about 0.117 once round.
    CHECK(moved == Approx(0.117F).margin(0.02F));
}

TEST_CASE("A hue shift of nothing returns the hue it was given", "[render][hue]") {
    const render::ColorCurveTable table{model::ColorCurves{}};
    CHECK_FALSE(table.hasShift());
    for (const float hue : {0.0F, 0.33F, 0.99F}) {
        CHECK(table.shiftedHue(hue) == Approx(hue));
    }
    // And a hue handed in from outside the circle still comes back inside it.
    CHECK(table.shiftedHue(1.25F) == Approx(0.25F).margin(1e-5));
}
