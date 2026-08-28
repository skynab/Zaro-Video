#include <cmath>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"
#include "zaro/core/render/PathRaster.h"
#include "zaro/core/render/RenderGraph.h"
#include "zaro/core/render/ShapeRaster.h"

#include "ModelFixtures.h"
#include "TestSources.h"

using namespace zaro;
using Catch::Approx;

namespace {

model::MaskPoint corner(double x, double y) {
    model::MaskPoint point;
    point.x = x;
    point.y = y;
    return point;
}

/// A square with corners, in output coordinates from the frame centre.
model::MaskPath square(double half) {
    model::MaskPath path;
    path.points = {corner(-half, -half), corner(half, -half), corner(half, half),
                   corner(-half, half)};
    return path;
}

float at(const std::vector<float>& coverage, std::int32_t width, std::int32_t x, std::int32_t y) {
    return coverage[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                    static_cast<std::size_t>(x)];
}

}  // namespace

TEST_CASE("A path of corners is a polygon", "[render][path]") {
    std::vector<float> coverage;
    render::rasteriseMaskPath(square(10.0), 0.0, false, 40, 40, coverage);

    // Inside the square, which is centred on a 40x40 frame.
    CHECK(at(coverage, 40, 20, 20) == Approx(1.0F));
    CHECK(at(coverage, 40, 12, 12) == Approx(1.0F));
    // Outside it.
    CHECK(at(coverage, 40, 2, 2) == Approx(0.0F));
    CHECK(at(coverage, 40, 38, 20) == Approx(0.0F));
}

TEST_CASE("Fewer than three points enclose nothing", "[render][path]") {
    model::MaskPath line;
    line.points = {corner(-5.0, 0.0), corner(5.0, 0.0)};
    CHECK_FALSE(line.isSet());

    std::vector<float> coverage;
    render::rasteriseMaskPath(line, 0.0, false, 16, 16, coverage);
    for (const float value : coverage) {
        CHECK(value == 0.0F);
    }
}

TEST_CASE("A handle bends the segment", "[render][path]") {
    // The difference between a polygon and a path: with an outgoing handle the
    // edge bows outwards, so a point outside the straight chord is now inside.
    model::MaskPath straight = square(10.0);
    std::vector<float> flatCoverage;
    render::rasteriseMaskPath(straight, 0.0, false, 40, 40, flatCoverage);

    model::MaskPath bowed = straight;
    // Bow the top edge upwards, away from the middle.
    bowed.points[0].outY = -14.0;
    bowed.points[1].inY = -14.0;
    std::vector<float> bowedCoverage;
    render::rasteriseMaskPath(bowed, 0.0, false, 40, 40, bowedCoverage);

    // A point just above the straight top edge: outside the polygon, inside the
    // bowed one.
    CHECK(at(flatCoverage, 40, 20, 6) == Approx(0.0F));
    CHECK(at(bowedCoverage, 40, 20, 6) > 0.9F);
}

TEST_CASE("The edge is antialiased", "[render][path]") {
    // A diagonal, where a hard fill would stair-step. Every pixel along it
    // should be partly covered rather than all-or-nothing.
    model::MaskPath triangle;
    triangle.points = {corner(-15.0, -15.0), corner(15.0, -15.0), corner(-15.0, 15.0)};

    std::vector<float> coverage;
    render::rasteriseMaskPath(triangle, 0.0, false, 40, 40, coverage);

    int partial = 0;
    for (const float value : coverage) {
        if (value > 0.05F && value < 0.95F) {
            ++partial;
        }
    }
    INFO(partial << " partly covered pixels");
    CHECK(partial > 10);
}

TEST_CASE("A path that winds twice is still inside", "[render][path]") {
    // Nonzero winding, not even-odd. A five-pointed star traced in one
    // direction winds twice around its own middle: under even-odd that middle
    // is *outside* and the star comes out hollow, which is a surprise every
    // time. Under nonzero it is filled, which is what somebody drawing a star
    // means by it.
    //
    // A bow tie does not test this -- the two halves only touch at a point, so
    // the pixel there is genuinely half covered whichever rule is used.
    model::MaskPath star;
    for (int k = 0; k < 5; ++k) {
        const double angle = (-90.0 + (static_cast<double>(k) * 144.0)) * std::numbers::pi / 180.0;
        star.points.push_back(corner(15.0 * std::cos(angle), 15.0 * std::sin(angle)));
    }

    std::vector<float> coverage;
    render::rasteriseMaskPath(star, 0.0, false, 40, 40, coverage);
    // The doubly-wound middle is solid.
    CHECK(at(coverage, 40, 20, 20) == Approx(1.0F));
    // And well outside the star is still empty.
    CHECK(at(coverage, 40, 1, 20) == Approx(0.0F));
}

TEST_CASE("Feather softens the edge without moving it", "[render][path]") {
    std::vector<float> hard;
    render::rasteriseMaskPath(square(10.0), 0.0, false, 60, 60, hard);
    std::vector<float> soft;
    render::rasteriseMaskPath(square(10.0), 8.0, false, 60, 60, soft);

    // The middle is still solid.
    CHECK(at(soft, 60, 30, 30) == Approx(1.0F).margin(0.02F));
    // The edge is no longer a step: just outside it there is now something.
    CHECK(at(hard, 60, 30, 18) == Approx(0.0F));
    CHECK(at(soft, 60, 30, 18) > 0.05F);
    // And just inside it there is now less than everything.
    CHECK(at(soft, 60, 30, 22) < 1.0F);
}

TEST_CASE("Inverting a path swaps inside for outside", "[render][path]") {
    std::vector<float> coverage;
    render::rasteriseMaskPath(square(10.0), 0.0, true, 40, 40, coverage);
    CHECK(at(coverage, 40, 20, 20) == Approx(0.0F));
    CHECK(at(coverage, 40, 2, 2) == Approx(1.0F));
}

TEST_CASE("Flattening follows the curve rather than a fixed count", "[render][path]") {
    // A nearly straight segment should cost almost nothing; a tight one should
    // be subdivided until it is smooth. A fixed count is wasteful on the first
    // and faceted on the second, and a mask's edge is where facets show.
    model::MaskPath gentle = square(10.0);
    model::MaskPath tight = square(10.0);
    tight.points[0].outX = 60.0;
    tight.points[0].outY = -60.0;
    tight.points[1].inX = -60.0;
    tight.points[1].inY = -60.0;

    CHECK(render::flatten(tight).size() > render::flatten(gentle).size() * 2);
}

TEST_CASE("A path mask reaches the picture and survives a round trip", "[render][path][io]") {
    testing::Fixture f;
    f.sequence().setSize(40, 40);
    testing::SolidFrameSource source{40, 40};
    source.define(f.longMedia, render::Rgba{1.0F, 1.0F, 1.0F, 1.0F});
    render::RenderGraph graph{source};

    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;

    model::Mask mask;
    mask.shape = model::MaskShape::Path;
    mask.path = square(10.0);
    REQUIRE(f.run(edit::makeSetMask(f.project, f.on(f.v1), clipId, mask)));

    auto frame = graph.composite(f.sequence(), f.at(10));
    REQUIRE(frame);
    // Inside the path the clip shows; outside it the frame is empty. A shape
    // the analytic masks cannot describe, drawn through the same compositor.
    CHECK(frame->at(20, 20).a == Approx(1.0F));
    CHECK(frame->at(2, 2).a == Approx(0.0F));

    auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    auto reloaded = io::loadProjectFromString(*text);
    REQUIRE(reloaded);
    const model::Mask& back =
        reloaded->project.findSequence(f.sequenceId)->findTrack(f.v1)->clips().front().mask;
    CHECK(back.shape == model::MaskShape::Path);
    REQUIRE(back.path.points.size() == 4);
    CHECK(back.path.points[1].x == Approx(10.0));

    SECTION("and handles come back with it") {
        model::Mask curved = mask;
        curved.path.points[0].outY = -14.0;
        REQUIRE(f.run(edit::makeSetMask(f.project, f.on(f.v1), clipId, curved)));
        auto curvedText = io::saveProjectToString(f.project);
        REQUIRE(curvedText);
        auto curvedBack = io::loadProjectFromString(*curvedText);
        REQUIRE(curvedBack);
        CHECK(curvedBack->project.findSequence(f.sequenceId)
                  ->findTrack(f.v1)
                  ->clips()
                  .front()
                  .mask.path.points[0]
                  .outY == Approx(-14.0));
    }
}

TEST_CASE("A shape converts to the path that draws it", "[render][path]") {
    // How a path gets made: start from the shape you have and bend it.
    model::Mask rectangle;
    rectangle.shape = model::MaskShape::Rectangle;
    rectangle.width = 20.0;
    rectangle.height = 20.0;

    const model::MaskPath asPath = render::pathForShape(rectangle);
    REQUIRE(asPath.points.size() == 4);

    std::vector<float> fromShape;
    std::vector<float> fromPath;
    render::rasteriseMaskPath(asPath, 0.0, false, 40, 40, fromPath);
    // The same coverage the analytic mask gives, so converting changes nothing
    // anybody can see -- which is what makes it safe to offer as one click.
    for (std::int32_t y = 0; y < 40; ++y) {
        for (std::int32_t x = 0; x < 40; ++x) {
            const float analytic = render::maskCoverage(rectangle, 40, 40, x, y);
            INFO("at " << x << "," << y);
            REQUIRE(std::fabs(at(fromPath, 40, x, y) - analytic) < 0.02F);
        }
    }
    static_cast<void>(fromShape);
}

TEST_CASE("An ellipse converts to four cubics that fit it", "[render][path]") {
    model::Mask ellipse;
    ellipse.shape = model::MaskShape::Ellipse;
    ellipse.width = 24.0;
    ellipse.height = 24.0;

    const model::MaskPath asPath = render::pathForShape(ellipse);
    REQUIRE(asPath.points.size() == 4);

    std::vector<float> coverage;
    render::rasteriseMaskPath(asPath, 0.0, false, 40, 40, coverage);
    // Compared against the analytic ellipse everywhere but the edge, where the
    // two antialias slightly differently.
    int disagreements = 0;
    for (std::int32_t y = 0; y < 40; ++y) {
        for (std::int32_t x = 0; x < 40; ++x) {
            const float analytic = render::maskCoverage(ellipse, 40, 40, x, y);
            if (std::fabs(at(coverage, 40, x, y) - analytic) > 0.25F) {
                ++disagreements;
            }
        }
    }
    INFO(disagreements << " pixels disagree");
    CHECK(disagreements == 0);
}
