#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/Compositing.h"

using namespace zaro;
using Catch::Approx;
using model::BlendMode;
using model::Transform;
using render::Rgba;
using render::RgbaImage;

namespace {

RgbaImage filled(std::int32_t width, std::int32_t height, const Rgba& colour) {
    RgbaImage image{width, height};
    image.fill(colour);
    return image;
}

/// Premultiplied: colour is already weighted by coverage.
Rgba premultiplied(float r, float g, float b, float a) {
    return Rgba{r * a, g * a, b * a, a};
}

}  // namespace

TEST_CASE("Over composites premultiplied alpha correctly", "[render][composite]") {
    RgbaImage destination = filled(4, 4, premultiplied(0.0F, 0.0F, 1.0F, 1.0F));   // opaque blue
    const RgbaImage source = filled(4, 4, premultiplied(1.0F, 0.0F, 0.0F, 0.5F));  // half red

    render::drawOver(source, destination);

    const Rgba result = destination.at(0, 0);
    // 0.5 red over opaque blue: red at half coverage, blue at the remaining half.
    CHECK(result.r == Approx(0.5F));
    CHECK(result.g == Approx(0.0F));
    CHECK(result.b == Approx(0.5F));
    CHECK(result.a == Approx(1.0F));
}

TEST_CASE("Opacity scales colour and coverage together", "[render][composite]") {
    RgbaImage destination{4, 4};
    const RgbaImage source = filled(4, 4, premultiplied(1.0F, 1.0F, 1.0F, 1.0F));

    render::drawOver(source, destination, 0.25);
    const Rgba result = destination.at(0, 0);
    CHECK(result.a == Approx(0.25F));
    // Still white when un-premultiplied: fading changes coverage, not hue.
    CHECK(result.r / result.a == Approx(1.0F));
}

TEST_CASE("A fully transparent source leaves the destination alone", "[render][composite]") {
    const Rgba background = premultiplied(0.2F, 0.4F, 0.6F, 1.0F);
    RgbaImage destination = filled(4, 4, background);
    const RgbaImage source{4, 4};  // transparent black

    render::drawOver(source, destination);
    CHECK(destination.at(0, 0) == background);
}

TEST_CASE("Blend modes", "[render][composite]") {
    RgbaImage destination = filled(2, 2, premultiplied(0.5F, 0.5F, 0.5F, 1.0F));
    const RgbaImage source = filled(2, 2, premultiplied(0.5F, 0.5F, 0.5F, 1.0F));

    SECTION("add brightens") {
        render::drawOver(source, destination, 1.0, BlendMode::Add);
        CHECK(destination.at(0, 0).r == Approx(1.0F));
    }

    SECTION("multiply darkens") {
        render::drawOver(source, destination, 1.0, BlendMode::Multiply);
        CHECK(destination.at(0, 0).r == Approx(0.25F));
    }

    SECTION("screen brightens less than add") {
        render::drawOver(source, destination, 1.0, BlendMode::Screen);
        CHECK(destination.at(0, 0).r == Approx(0.75F));
    }
}

TEST_CASE("An identity transform is an exact copy", "[render][composite][transform]") {
    // Any half-pixel error in the sampler shows up here as a blur, which is the
    // most common way a resampler is quietly wrong.
    RgbaImage source{8, 8};
    for (std::int32_t y = 0; y < 8; ++y) {
        for (std::int32_t x = 0; x < 8; ++x) {
            const float value = static_cast<float>(x * 8 + y) / 64.0F;
            source.at(x, y) = Rgba{value, value, value, 1.0F};
        }
    }
    RgbaImage destination{8, 8};
    render::drawTransformed(source, destination, Transform{});

    for (std::int32_t y = 0; y < 8; ++y) {
        for (std::int32_t x = 0; x < 8; ++x) {
            INFO("pixel " << x << "," << y);
            CHECK(destination.at(x, y).r == Approx(source.at(x, y).r).margin(1e-5));
        }
    }
}

TEST_CASE("Scale enlarges about the centre", "[render][composite][transform]") {
    RgbaImage source{4, 4};
    source.fill(premultiplied(1.0F, 0.0F, 0.0F, 1.0F));
    RgbaImage destination{16, 16};

    Transform transform;
    transform.scaleX = 2.0;
    transform.scaleY = 2.0;
    render::drawTransformed(source, destination, transform);

    // A 4x4 source scaled 2x covers 8x8 centred in a 16x16 frame: pixels 4..11.
    CHECK(destination.at(8, 8).a == Approx(1.0F));
    CHECK(destination.at(5, 5).a == Approx(1.0F));
    CHECK(destination.at(0, 0).a == Approx(0.0F));
    CHECK(destination.at(15, 15).a == Approx(0.0F));
}

TEST_CASE("Position moves the clip", "[render][composite][transform]") {
    RgbaImage source{4, 4};
    source.fill(premultiplied(0.0F, 1.0F, 0.0F, 1.0F));
    RgbaImage destination{16, 16};

    Transform transform;
    transform.positionX = 4.0;
    render::drawTransformed(source, destination, transform);

    // Centred at 8+4=12, so it covers roughly x in 10..13.
    CHECK(destination.at(12, 8).a == Approx(1.0F));
    CHECK(destination.at(6, 8).a == Approx(0.0F));
}

TEST_CASE("Rotating by 360 degrees returns the original", "[render][composite][transform]") {
    RgbaImage source{8, 8};
    source.fill(premultiplied(0.3F, 0.6F, 0.9F, 1.0F));

    RgbaImage once{8, 8};
    RgbaImage full{8, 8};
    Transform none;
    Transform turn;
    turn.rotationDegrees = 360.0;
    render::drawTransformed(source, once, none);
    render::drawTransformed(source, full, turn);

    CHECK(full.at(4, 4).r == Approx(once.at(4, 4).r).margin(1e-4));
    CHECK(full.at(4, 4).a == Approx(once.at(4, 4).a).margin(1e-4));
}

TEST_CASE("Sampling outside the source is transparent, not smeared",
          "[render][composite][transform]") {
    // Clamping at the edge instead would streak the border pixel outwards,
    // which is visible on anything scaled up or rotated.
    RgbaImage source{4, 4};
    source.fill(premultiplied(1.0F, 1.0F, 1.0F, 1.0F));

    CHECK(source.sampleBilinear(-5.0F, 2.0F).a == 0.0F);
    CHECK(source.sampleBilinear(2.0F, 99.0F).a == 0.0F);
    CHECK(source.sampleBilinear(1.0F, 1.0F).a == Approx(1.0F));
}

TEST_CASE("A zero scale draws nothing rather than dividing by zero",
          "[render][composite][transform]") {
    RgbaImage source{4, 4};
    source.fill(premultiplied(1.0F, 1.0F, 1.0F, 1.0F));
    RgbaImage destination{8, 8};

    Transform transform;
    transform.scaleX = 0.0;
    render::drawTransformed(source, destination, transform);
    CHECK(destination.at(4, 4).a == 0.0F);
}

TEST_CASE("Zero opacity draws nothing", "[render][composite][transform]") {
    RgbaImage source{4, 4};
    source.fill(premultiplied(1.0F, 1.0F, 1.0F, 1.0F));
    RgbaImage destination{4, 4};

    Transform transform;
    transform.opacity = 0.0;
    render::drawTransformed(source, destination, transform);
    CHECK(destination.at(2, 2).a == 0.0F);
}
