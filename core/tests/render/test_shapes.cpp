#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/render/RenderGraph.h"
#include "zaro/core/render/ShapeRaster.h"

#include "ModelFixtures.h"
#include "TestSources.h"

using namespace zaro;
using Catch::Approx;
using zaro::testing::Fixture;
using zaro::testing::SolidFrameSource;

namespace {

render::Rgba opaque(float r, float g, float b) {
    return render::Rgba{r, g, b, 1.0F};
}

model::Graphic rectangle(double width, double height) {
    model::Graphic out;
    out.kind = model::GraphicKind::Rectangle;
    out.width = width;
    out.height = height;
    return out;
}

}  // namespace

TEST_CASE("A shape is centred on the frame by default", "[render][shape]") {
    render::RgbaImage image{64, 64};
    render::drawShape(rectangle(20, 10), image);

    CHECK(image.at(32, 32).a == Approx(1.0F));
    // Ten across and five down from the centre is the corner; well outside is
    // empty.
    CHECK(image.at(32, 5).a == Approx(0.0F));
    CHECK(image.at(5, 32).a == Approx(0.0F));
    CHECK(image.at(0, 0).a == Approx(0.0F));
}

TEST_CASE("A shape moves in the same coordinates a transform uses", "[render][shape]") {
    // Output pixels from the centre of the frame. A shape and a clip have to
    // mean the same thing by "forty pixels right", or the motion controls stop
    // making sense the moment they are pointed at a graphic.
    model::Graphic graphic = rectangle(10, 10);
    graphic.centreX = 16.0;
    graphic.centreY = -8.0;

    render::RgbaImage image{64, 64};
    render::drawShape(graphic, image);
    CHECK(image.at(48, 24).a == Approx(1.0F));
    CHECK(image.at(32, 32).a == Approx(0.0F));
}

TEST_CASE("A shape is premultiplied and its colour survives", "[render][shape]") {
    model::Graphic graphic = rectangle(20, 20);
    graphic.red = 0.8;
    graphic.green = 0.4;
    graphic.blue = 0.1;
    graphic.alpha = 0.5;

    render::RgbaImage image{32, 32};
    render::drawShape(graphic, image);

    const render::Rgba& pixel = image.at(16, 16);
    CHECK(pixel.a == Approx(0.5F));
    // Premultiplied: the stored colour is the colour times coverage, so
    // dividing alpha back out gives what was asked for.
    CHECK(pixel.r / pixel.a == Approx(0.8F).margin(1e-5));
    CHECK(pixel.g / pixel.a == Approx(0.4F).margin(1e-5));
    CHECK(pixel.b / pixel.a == Approx(0.1F).margin(1e-5));
}

TEST_CASE("Edges are antialiased even with no feather", "[render][shape]") {
    // A stair-stepped edge is the first thing anyone notices about a generated
    // graphic, so a hard edge still gets a pixel of ramp.
    model::Graphic graphic = rectangle(21, 21);
    render::RgbaImage image{64, 64};
    render::drawShape(graphic, image);

    bool sawPartial = false;
    for (std::int32_t x = 0; x < 64; ++x) {
        const float alpha = image.at(x, 32).a;
        if (alpha > 0.05F && alpha < 0.95F) {
            sawPartial = true;
        }
    }
    CHECK(sawPartial);
}

TEST_CASE("Feather widens the edge without moving it", "[render][shape]") {
    model::Graphic hard = rectangle(30, 30);
    model::Graphic soft = hard;
    soft.feather = 8.0;

    render::RgbaImage hardImage{64, 64};
    render::RgbaImage softImage{64, 64};
    render::drawShape(hard, hardImage);
    render::drawShape(soft, softImage);

    // The centre is solid either way, and the edge itself -- where coverage is
    // a half -- stays where it was.
    CHECK(hardImage.at(32, 32).a == Approx(1.0F));
    CHECK(softImage.at(32, 32).a == Approx(1.0F));

    const auto halfway = [](const render::RgbaImage& image) {
        for (std::int32_t x = 32; x < 64; ++x) {
            if (image.at(x, 32).a <= 0.5F) {
                return x;
            }
        }
        return 64;
    };
    CHECK(std::abs(halfway(hardImage) - halfway(softImage)) <= 1);

    // But the soft one takes far longer to get there.
    const auto width = [](const render::RgbaImage& image) {
        std::int32_t count = 0;
        for (std::int32_t x = 32; x < 64; ++x) {
            const float alpha = image.at(x, 32).a;
            if (alpha > 0.02F && alpha < 0.98F) {
                ++count;
            }
        }
        return count;
    };
    CHECK(width(softImage) > width(hardImage) + 3);
}

TEST_CASE("An ellipse is round, not a rectangle", "[render][shape]") {
    model::Graphic graphic = rectangle(40, 40);
    graphic.kind = model::GraphicKind::Ellipse;

    render::RgbaImage image{64, 64};
    render::drawShape(graphic, image);

    // Centre solid, the corners of its bounding box empty -- which is the
    // whole difference between the two shapes.
    CHECK(image.at(32, 32).a == Approx(1.0F));
    CHECK(image.at(32 + 19, 32).a > 0.5F);       // on the horizontal axis, inside
    CHECK(image.at(32 + 18, 32 + 18).a < 0.1F);  // the corner, outside
}

TEST_CASE("A rounded rectangle loses its corners and keeps its sides", "[render][shape]") {
    model::Graphic graphic = rectangle(40, 40);
    graphic.cornerRadius = 12.0;

    render::RgbaImage image{64, 64};
    render::drawShape(graphic, image);

    CHECK(image.at(32 + 19, 32).a > 0.5F);       // middle of a side
    CHECK(image.at(32 + 19, 32 + 19).a < 0.1F);  // the corner is gone

    // A radius larger than the shape is clamped rather than folding it inside
    // out: half the smaller side is a circle, and there is nothing beyond that
    // for a radius to mean.
    graphic.cornerRadius = 500.0;
    render::drawShape(graphic, image);
    CHECK(image.at(32, 32).a == Approx(1.0F));
    CHECK(image.at(32 + 19, 32 + 19).a < 0.1F);
}

TEST_CASE("A shape with no size draws nothing", "[render][shape]") {
    model::Graphic graphic = rectangle(0.0, 40.0);
    render::RgbaImage image{32, 32};
    render::drawShape(graphic, image);
    for (std::int32_t y = 0; y < 32; ++y) {
        for (std::int32_t x = 0; x < 32; ++x) {
            REQUIRE(image.at(x, y).a == 0.0F);
        }
    }
}

TEST_CASE("Drawing a shape clears what was there", "[render][shape]") {
    // A graphic clip's picture is the shape and transparency, not the shape
    // over whatever the buffer happened to hold.
    render::RgbaImage image{32, 32};
    image.fill(render::Rgba{1.0F, 0.0F, 0.0F, 1.0F});
    render::drawShape(rectangle(8, 8), image);
    CHECK(image.at(0, 0).a == 0.0F);
    CHECK(image.at(0, 0).r == 0.0F);
    CHECK(image.at(16, 16).a == Approx(1.0F));
}

TEST_CASE("Graphic kinds round trip through their names", "[render][shape]") {
    for (const model::GraphicKind kind :
         {model::GraphicKind::None, model::GraphicKind::Rectangle, model::GraphicKind::Ellipse}) {
        CHECK(model::graphicKindFromString(model::toString(kind)) == kind);
    }
    CHECK(model::graphicKindFromString("somethingElse") == model::GraphicKind::None);
    CHECK(model::graphicKindFromString(nullptr) == model::GraphicKind::None);
}

TEST_CASE("A mask lets everything through when there is none", "[render][mask]") {
    const model::Mask none;
    CHECK_FALSE(none.isSet());
    CHECK(render::maskCoverage(none, 64, 64, 0, 0) == Approx(1.0F));
    CHECK(render::maskCoverage(none, 64, 64, 32, 32) == Approx(1.0F));
}

TEST_CASE("A mask and a shape of the same size cover the same pixels", "[render][mask]") {
    // They share the geometry, which is what makes the two controls mean the
    // same thing when someone sees them side by side.
    model::Graphic shape;
    shape.kind = model::GraphicKind::Ellipse;
    shape.width = 40;
    shape.height = 24;
    shape.centreX = 6;
    shape.centreY = -4;

    model::Mask mask;
    mask.shape = model::MaskShape::Ellipse;
    mask.width = shape.width;
    mask.height = shape.height;
    mask.centreX = shape.centreX;
    mask.centreY = shape.centreY;

    for (std::int32_t y = 0; y < 64; y += 3) {
        for (std::int32_t x = 0; x < 64; x += 3) {
            REQUIRE(render::maskCoverage(mask, 64, 64, x, y) ==
                    Approx(render::shapeCoverage(shape, 64, 64, x, y)).margin(1e-6));
        }
    }
}

TEST_CASE("Inverting a mask gives its complement", "[render][mask]") {
    // A vignette and a spotlight are the same mask with this flipped. Having to
    // draw the complement by hand is how people end up with two masks that must
    // be kept agreeing.
    model::Mask mask;
    mask.shape = model::MaskShape::Rectangle;
    mask.width = 20;
    mask.height = 20;
    mask.feather = 4;

    model::Mask flipped = mask;
    flipped.inverted = true;

    for (std::int32_t y = 0; y < 64; y += 2) {
        for (std::int32_t x = 0; x < 64; x += 2) {
            const float straight = render::maskCoverage(mask, 64, 64, x, y);
            const float inverse = render::maskCoverage(flipped, 64, 64, x, y);
            REQUIRE(straight + inverse == Approx(1.0F).margin(1e-6));
        }
    }
}

TEST_CASE("A mask stays put when its clip moves", "[render][graph][mask]") {
    // The reason a mask is in output coordinates. A mask that travelled with
    // its clip would be a crop, which is a different tool: this one is a window
    // on the screen that a clip can be moved behind.
    Fixture f;
    f.sequence().setSize(64, 64);
    SolidFrameSource source{64, 64};
    source.define(f.longMedia, opaque(1.0F, 1.0F, 1.0F));
    render::RenderGraph graph{source};

    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    const model::ClipId id = f.track(f.v1).clips().front().id;

    model::Mask mask;
    mask.shape = model::MaskShape::Rectangle;
    mask.width = 16;
    mask.height = 16;
    f.track(f.v1).find(id)->mask = mask;

    const auto masked = graph.composite(f.sequence(), f.at(10));
    REQUIRE(masked);
    CHECK(masked->at(32, 32).a == Approx(1.0F));
    CHECK(masked->at(8, 8).a == Approx(0.0F));

    // Move the clip well to the right. The window does not move with it.
    model::Transform moved;
    moved.positionX = 20.0;
    REQUIRE(f.run(edit::makeSetTransform(f.project, f.on(f.v1), id, moved)));
    const auto shifted = graph.composite(f.sequence(), f.at(10));
    REQUIRE(shifted);
    CHECK(shifted->at(32, 32).a == Approx(1.0F));
    CHECK(shifted->at(52, 32).a == Approx(0.0F));
}
