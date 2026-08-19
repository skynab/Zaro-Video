#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/io/CubeLut.h"
#include "zaro/core/render/ColorPipeline.h"
#include "zaro/core/render/CurveTable.h"
#include "zaro/core/render/LutTable.h"

using namespace zaro;
using Catch::Approx;

namespace {

model::ToneCurve curveThrough(std::initializer_list<model::CurvePoint> points) {
    model::ToneCurve curve;
    for (const model::CurvePoint& point : points) {
        curve.set(point);
    }
    return curve;
}

}  // namespace

TEST_CASE("An empty or diagonal curve is the identity", "[render][curves]") {
    CHECK(model::ToneCurve{}.isIdentity());
    // One point cannot describe a mapping, so it is still identity rather than
    // a constant.
    CHECK(curveThrough({{0.5, 0.9}}).isIdentity());
    CHECK(curveThrough({{0.0, 0.0}, {1.0, 1.0}}).isIdentity());
    CHECK_FALSE(curveThrough({{0.0, 0.0}, {0.5, 0.7}, {1.0, 1.0}}).isIdentity());

    const model::ToneCurve diagonal = curveThrough({{0.0, 0.0}, {0.4, 0.4}, {1.0, 1.0}});
    for (double x = 0.0; x <= 1.0; x += 0.05) {
        CHECK(diagonal.valueAt(x) == Approx(x).margin(1e-9));
    }
}

TEST_CASE("A curve passes through its own control points", "[render][curves]") {
    const model::ToneCurve curve =
        curveThrough({{0.0, 0.1}, {0.25, 0.2}, {0.6, 0.85}, {1.0, 0.95}});
    for (const model::CurvePoint& point : curve.points()) {
        CHECK(curve.valueAt(point.x) == Approx(point.y).margin(1e-9));
    }
}

TEST_CASE("A monotonic set of points gives a monotonic curve", "[render][curves]") {
    // The reason for the Fritsch-Carlson spline. A natural cubic through these
    // points overshoots badly at the step, and an overshooting tone curve puts
    // a dark halo above a highlight and can invert a gradient.
    const model::ToneCurve steep =
        curveThrough({{0.0, 0.0}, {0.4, 0.05}, {0.45, 0.9}, {0.5, 0.92}, {1.0, 1.0}});

    double previous = -1.0;
    for (int i = 0; i <= 1000; ++i) {
        const double value = steep.valueAt(i / 1000.0);
        CHECK(value >= previous - 1e-12);
        // And never leaves the range its own points describe.
        CHECK(value >= -1e-9);
        CHECK(value <= 1.0 + 1e-9);
        previous = value;
    }
}

TEST_CASE("A curve holds its ends rather than extrapolating", "[render][curves]") {
    const model::ToneCurve curve = curveThrough({{0.2, 0.3}, {0.8, 0.7}});
    CHECK(curve.valueAt(0.0) == Approx(0.3));
    CHECK(curve.valueAt(-1.0) == Approx(0.3));
    CHECK(curve.valueAt(1.0) == Approx(0.7));
    CHECK(curve.valueAt(50.0) == Approx(0.7));
}

TEST_CASE("Points are sorted and deduplicated by x", "[render][curves]") {
    model::ToneCurve curve;
    curve.set({0.8, 0.9});
    curve.set({0.2, 0.1});
    curve.set({0.5, 0.5});
    REQUIRE(curve.size() == 3);
    CHECK(curve.points()[0].x == Approx(0.2));
    CHECK(curve.points()[2].x == Approx(0.8));

    // Two points at one x describe a vertical segment, which is not a function.
    curve.set({0.5, 0.75});
    REQUIRE(curve.size() == 3);
    CHECK(curve.valueAt(0.5) == Approx(0.75));

    CHECK(curve.removeAt(0.5));
    CHECK_FALSE(curve.removeAt(0.5));
    CHECK(curve.size() == 2);
}

TEST_CASE("The table's index covers everything from black to far above white", "[render][curves]") {
    // Linear light has no upper bound: a highlight may be many times white.
    // The index has to map all of it into the table without running out.
    CHECK(render::CurveTable::indexFor(0.0F) == Approx(0.0F));
    CHECK(render::CurveTable::indexFor(1.0F) == Approx(std::sqrt(0.5F)));
    CHECK(render::CurveTable::indexFor(1e6F) < 1.0F);
    CHECK(render::CurveTable::indexFor(1e6F) > 0.999F);
    // Negative light has no meaning; it must not become a negative index.
    CHECK(render::CurveTable::indexFor(-0.5F) == Approx(0.0F));

    // Monotonic, or the table would fold two brightnesses onto one entry.
    float previous = -1.0F;
    for (float v = 0.0F; v < 20.0F; v += 0.05F) {
        const float index = render::CurveTable::indexFor(v);
        CHECK(index >= previous);
        previous = index;
    }

    // Round trips through its own inverse.
    for (const float v : {0.001F, 0.018F, 0.18F, 1.0F, 4.0F, 12.0F}) {
        CHECK(render::CurveTable::linearFor(render::CurveTable::indexFor(v)) ==
              Approx(v).epsilon(0.001));
    }
}

TEST_CASE("The index spends its resolution where the picture is", "[render][curves]") {
    // A linear index would give everything below a thousandth of white a single
    // entry, and the shadows are exactly where a tone curve is read most
    // closely.
    const float shadow = render::CurveTable::indexFor(0.001F) * (render::CurveTable::kEntries - 1);
    CHECK(shadow > 20.0F);
    const float middleGrey =
        render::CurveTable::indexFor(0.18F) * (render::CurveTable::kEntries - 1);
    CHECK(middleGrey > 350.0F);
    CHECK(middleGrey < 450.0F);
}

TEST_CASE("An identity curve set bakes to an identity table", "[render][curves]") {
    const render::CurveTable table{model::ToneCurves{}, media::TransferFunction::BT709};
    CHECK(table.isIdentity());
    // And applying it is exactly a no-op, not an approximate one.
    for (const float v : {0.0F, 0.18F, 1.0F, 3.0F}) {
        CHECK(table.apply(v, 0) == v);
    }
}

TEST_CASE("A curve is applied in the encoded domain", "[render][curves]") {
    // The point of the design. A curve that lifts the encoded midpoint from
    // 0.5 to 0.6 has to do so where the signal is *shown*, not in linear light
    // where 0.5 is nearly two stops above middle grey.
    model::ToneCurves curves;
    curves.master = curveThrough({{0.0, 0.0}, {0.5, 0.6}, {1.0, 1.0}});
    const render::CurveTable table{curves, media::TransferFunction::BT709};
    REQUIRE_FALSE(table.isIdentity());

    // Take the linear value that encodes to 0.5, push it through the table, and
    // it should come back as the linear value that encodes to 0.6.
    const float linearIn = render::toLinearScalar(0.5F, media::TransferFunction::BT709);
    const float expected = render::toLinearScalar(0.6F, media::TransferFunction::BT709);
    CHECK(table.apply(linearIn, 0) == Approx(expected).epsilon(0.01));

    // Black and white are pinned by the curve's own endpoints.
    CHECK(table.apply(0.0F, 0) == Approx(0.0F).margin(1e-4));
    CHECK(table.apply(1.0F, 1) == Approx(1.0F).epsilon(0.01));
}

TEST_CASE("Per-channel curves act on their own channel only", "[render][curves]") {
    model::ToneCurves curves;
    curves.red = curveThrough({{0.0, 0.0}, {0.5, 0.7}, {1.0, 1.0}});
    const render::CurveTable table{curves, media::TransferFunction::BT709};

    const float mid = render::toLinearScalar(0.5F, media::TransferFunction::BT709);
    CHECK(table.apply(mid, 0) > mid * 1.2F);
    CHECK(table.apply(mid, 1) == Approx(mid).epsilon(0.01));
    CHECK(table.apply(mid, 2) == Approx(mid).epsilon(0.01));
}

TEST_CASE("The master curve is applied after the per-channel ones", "[render][curves]") {
    // A master curve is a statement about the picture's tones, and the picture
    // is what the per-channel curves have already made it. Composed the other
    // way the two orders give different answers, so the order is part of the
    // contract.
    model::ToneCurves both;
    both.red = curveThrough({{0.0, 0.0}, {0.5, 0.7}, {1.0, 1.0}});
    both.master = curveThrough({{0.0, 0.0}, {0.5, 0.25}, {1.0, 1.0}});
    const render::CurveTable table{both, media::TransferFunction::BT709};

    const float mid = render::toLinearScalar(0.5F, media::TransferFunction::BT709);
    const double afterChannel = both.red.valueAt(0.5);
    const double afterMaster = both.master.valueAt(afterChannel);
    const float expected =
        render::toLinearScalar(static_cast<float>(afterMaster), media::TransferFunction::BT709);
    CHECK(table.apply(mid, 0) == Approx(expected).epsilon(0.01));
}

TEST_CASE("Tables are cached until the curves change", "[render][curves]") {
    // Building one is thousands of spline evaluations and as many pows: fine
    // once, ruinous per frame.
    render::CurveTableCache cache;
    model::ToneCurves curves;
    curves.master = curveThrough({{0.0, 0.0}, {0.5, 0.6}, {1.0, 1.0}});

    for (int i = 0; i < 50; ++i) {
        const auto& table = cache.tableFor(7, curves, media::TransferFunction::BT709);
        CHECK_FALSE(table.isIdentity());
    }
    CHECK(cache.builds() == 1);

    // A different clip is a different table.
    (void)cache.tableFor(8, curves, media::TransferFunction::BT709);
    CHECK(cache.builds() == 2);

    // Changing the curve rebuilds, and nothing else does.
    curves.master.set({0.5, 0.4});
    (void)cache.tableFor(7, curves, media::TransferFunction::BT709);
    CHECK(cache.builds() == 3);
    (void)cache.tableFor(7, curves, media::TransferFunction::BT709);
    CHECK(cache.builds() == 3);

    // As does the transfer function the curve is baked against.
    (void)cache.tableFor(7, curves, media::TransferFunction::SRGB);
    CHECK(cache.builds() == 4);
}

TEST_CASE("A baked LUT round trips the identity", "[render][lut]") {
    // The bake is a transfer round trip and a resample. An identity LUT has to
    // survive both, or every graded shot carries a small error that nobody put
    // there.
    std::ostringstream text;
    const int size = 17;
    text << "LUT_3D_SIZE " << size << "\n";
    for (int b = 0; b < size; ++b) {
        for (int g = 0; g < size; ++g) {
            for (int r = 0; r < size; ++r) {
                text << (static_cast<double>(r) / (size - 1)) << " "
                     << (static_cast<double>(g) / (size - 1)) << " "
                     << (static_cast<double>(b) / (size - 1)) << "\n";
            }
        }
    }
    const auto cube = io::CubeLut::parse(text.str());
    REQUIRE(cube);
    const render::LutTable table{*cube, media::TransferFunction::BT709};
    REQUIRE(table.isValid());

    for (const float value : {0.0F, 0.05F, 0.18F, 0.5F, 1.0F}) {
        float r = value;
        float g = value * 0.6F;
        float b = value * 0.3F;
        const float wasR = r;
        const float wasG = g;
        const float wasB = b;
        table.apply(r, g, b, 1.0F);
        // Tight on purpose: an identity LUT that dimmed the picture by even a
        // code value would put an error into every graded shot that nobody
        // could find by looking at the LUT.
        CHECK(r == Approx(wasR).margin(0.002));
        CHECK(g == Approx(wasG).margin(0.002));
        CHECK(b == Approx(wasB).margin(0.002));
    }
}

TEST_CASE("A baked LUT is applied in the encoded domain", "[render][lut]") {
    // The same reasoning as the curves. A LUT that lifts encoded 0.5 to 0.6 has
    // to do it where the signal is shown, not in linear light.
    std::ostringstream text;
    text << "LUT_1D_SIZE 3\n";
    text << "0.0 0.0 0.0\n";
    text << "0.6 0.6 0.6\n";
    text << "1.0 1.0 1.0\n";
    const auto cube = io::CubeLut::parse(text.str());
    REQUIRE(cube);
    const render::LutTable table{*cube, media::TransferFunction::BT709};

    float r = render::toLinearScalar(0.5F, media::TransferFunction::BT709);
    float g = r;
    float b = r;
    table.apply(r, g, b, 1.0F);
    const float expected = render::toLinearScalar(0.6F, media::TransferFunction::BT709);
    CHECK(r == Approx(expected).epsilon(0.03));
}

TEST_CASE("The amount control dials a look back", "[render][lut]") {
    std::ostringstream text;
    text << "LUT_1D_SIZE 2\n0.5 0.5 0.5\n0.5 0.5 0.5\n";  // everything to mid grey
    const auto cube = io::CubeLut::parse(text.str());
    REQUIRE(cube);
    const render::LutTable table{*cube, media::TransferFunction::BT709};

    const float start = 0.9F;
    float full = start;
    float g = start;
    float b = start;
    table.apply(full, g, b, 1.0F);

    float half = start;
    float g2 = start;
    float b2 = start;
    table.apply(half, g2, b2, 0.5F);

    float none = start;
    float g3 = start;
    float b3 = start;
    table.apply(none, g3, b3, 0.0F);

    CHECK(none == Approx(start));
    CHECK(half == Approx(start + ((full - start) * 0.5F)).epsilon(0.01));
    CHECK(full < start);
}

TEST_CASE("A LUT covers highlights above white", "[render][lut]") {
    // Linear light has no ceiling. A cube indexed linearly would run out at
    // white and clip every highlight to whatever the last entry says.
    std::ostringstream text;
    const int size = 5;
    text << "LUT_3D_SIZE " << size << "\n";
    for (int b = 0; b < size; ++b) {
        for (int g = 0; g < size; ++g) {
            for (int r = 0; r < size; ++r) {
                text << (static_cast<double>(r) / (size - 1)) << " "
                     << (static_cast<double>(g) / (size - 1)) << " "
                     << (static_cast<double>(b) / (size - 1)) << "\n";
            }
        }
    }
    const auto cube = io::CubeLut::parse(text.str());
    REQUIRE(cube);
    const render::LutTable table{*cube, media::TransferFunction::BT709};

    for (const float value : {1.0F, 2.0F, 8.0F}) {
        float r = value;
        float g = value;
        float b = value;
        table.apply(r, g, b, 1.0F);
        CHECK(std::isfinite(r));
        // The LUT clamps to its domain, so everything at or above white comes
        // back at white rather than as a black hole.
        CHECK(r == Approx(1.0F).margin(0.02));
    }
}

TEST_CASE("The LUT cache reads a file once and remembers a failure", "[render][lut]") {
    render::LutCache cache;
    CHECK(cache.tableFor("/definitely/not/a/lut.cube", media::TransferFunction::BT709) == nullptr);
    CHECK(cache.loads() == 1);
    // Remembered, not retried: a missing LUT would otherwise be a file-system
    // call per clip per frame.
    CHECK(cache.tableFor("/definitely/not/a/lut.cube", media::TransferFunction::BT709) == nullptr);
    CHECK(cache.loads() == 1);
    CHECK_FALSE(cache.errorFor("/definitely/not/a/lut.cube").empty());

    // An empty path is not a LUT at all, and asks nothing of the file system.
    CHECK(cache.tableFor("", media::TransferFunction::BT709) == nullptr);
    CHECK(cache.loads() == 1);
}
