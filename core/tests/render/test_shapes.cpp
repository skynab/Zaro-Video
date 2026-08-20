#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/ShapeRaster.h"

using namespace zaro;
using Catch::Approx;

namespace {

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
