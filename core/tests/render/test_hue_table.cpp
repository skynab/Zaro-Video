// Hue curves: the wrap, and the neutral in the middle.

#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/HueTable.h"

using namespace zaro;
using Catch::Approx;

namespace {

model::HueCurves flat(double y) {
    model::HueCurves out;
    for (const double x : {0.0, 0.25, 0.5, 0.75}) {
        out.saturation.set(model::CurvePoint{x, y});
    }
    return out;
}

}  // namespace

TEST_CASE("A hue curve with no points changes nothing", "[render][hue]") {
    const render::HueTable table{model::HueCurves{}};
    CHECK(table.isIdentity());
    for (const float hue : {0.0F, 0.1F, 0.5F, 0.99F}) {
        CHECK(table.saturationAt(hue) == Approx(1.0F));
    }
}

TEST_CASE("Half way up the curve is no change at all", "[render][hue]") {
    // 0.5 is neutral, not 0: the value is a multiplier and the curve has to be
    // able to go both ways, so the middle of the range means leave it alone.
    const render::HueTable table{flat(0.5)};
    CHECK_FALSE(table.isIdentity());
    for (const float hue : {0.0F, 0.2F, 0.6F, 0.95F}) {
        CHECK(table.saturationAt(hue) == Approx(1.0F).margin(1e-5));
    }
}

TEST_CASE("The ends of the curve are the multipliers they say", "[render][hue]") {
    CHECK(render::HueTable{flat(1.0)}.saturationAt(0.3F) == Approx(2.0F).margin(1e-5));
    CHECK(render::HueTable{flat(0.0)}.saturationAt(0.3F) == Approx(0.0F).margin(1e-5));
}

TEST_CASE("A hue curve wraps rather than seaming at red", "[render][hue]") {
    // The property the table exists for. A point just below the top of the
    // circle and one just above the bottom are neighbours, and the curve
    // between them has to run the short way -- across red -- not the long way
    // through every other hue.
    model::HueCurves curves;
    curves.saturation.set(model::CurvePoint{0.95, 1.0});
    curves.saturation.set(model::CurvePoint{0.05, 1.0});
    // Somewhere on the far side, pulled down, so the curve is not simply flat.
    curves.saturation.set(model::CurvePoint{0.5, 0.0});
    const render::HueTable table{curves};

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
    const render::HueTable table{flat(1.0)};
    CHECK(table.saturationAt(1.0F) == Approx(table.saturationAt(0.0F)).margin(1e-5));
    CHECK(table.saturationAt(1.25F) == Approx(table.saturationAt(0.25F)).margin(1e-5));
    CHECK(table.saturationAt(-0.25F) == Approx(table.saturationAt(0.75F)).margin(1e-5));
}

TEST_CASE("A hue curve never asks for a negative saturation", "[render][hue]") {
    // A negative multiplier pushes a pixel through grey and out the other side,
    // which is a different colour rather than a less saturated one. The
    // interpolation does not overshoot, and the bake clamps regardless.
    model::HueCurves curves;
    curves.saturation.set(model::CurvePoint{0.0, 0.0});
    curves.saturation.set(model::CurvePoint{0.1, 1.0});
    curves.saturation.set(model::CurvePoint{0.2, 0.0});
    const render::HueTable table{curves};
    for (int i = 0; i < 256; ++i) {
        CHECK(table.saturationAt(static_cast<float>(i) / 256.0F) >= 0.0F);
    }
}
