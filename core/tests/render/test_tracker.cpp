#include <cmath>
#include <random>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/Tracker.h"

using namespace zaro;
using Catch::Approx;

namespace {

/// A picture with enough going on in it to track: a smooth blob on a field of
/// fixed noise. Noise alone is the easiest thing in the world to track and
/// nothing looks like it; a blob alone has a flat interior, which is what real
/// footage mostly is. Both together is the honest case.
render::RgbaImage textured(std::int32_t width, std::int32_t height, double shiftX, double shiftY) {
    render::RgbaImage image{width, height};
    std::mt19937 noise{7};
    std::uniform_real_distribution<double> grain{-0.02, 0.02};
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            // Sampled at a shifted position rather than copied and moved, so a
            // fractional shift is a real fractional shift of the same picture.
            const double sx = static_cast<double>(x) - shiftX;
            const double sy = static_cast<double>(y) - shiftY;
            const double dx = (sx - 120.0) / 30.0;
            const double dy = (sy - 90.0) / 30.0;
            const double blob = std::exp(-((dx * dx) + (dy * dy)));
            const double stripes = 0.25 * std::sin(sx * 0.4) * std::cos(sy * 0.37);
            const auto value = static_cast<float>(0.3 + blob + stripes + grain(noise));
            image.at(x, y) = render::Rgba{value, value, value, 1.0F};
        }
    }
    return image;
}

render::PatchWindow around(double x, double y) {
    render::PatchWindow window;
    window.centreX = x;
    window.centreY = y;
    window.halfWidth = 45.0;
    window.halfHeight = 45.0;
    window.search = 20.0;
    return window;
}

}  // namespace

TEST_CASE("a whole-pixel shift is found exactly", "[tracker]") {
    const render::RgbaImage first = textured(320, 240, 0.0, 0.0);
    const render::RgbaImage second = textured(320, 240, 7.0, -5.0);

    const render::PatchTrack track = render::trackPatch(first, second, around(120.0, 90.0));
    REQUIRE(track.usable);
    CHECK(track.confidence > 0.95);
    CHECK(track.dx == Approx(7.0).margin(0.2));
    CHECK(track.dy == Approx(-5.0).margin(0.2));
}

TEST_CASE("a fractional shift is found between pixels", "[tracker]") {
    const render::RgbaImage first = textured(320, 240, 0.0, 0.0);
    const render::RgbaImage second = textured(320, 240, 3.5, 2.25);

    const render::PatchTrack track = render::trackPatch(first, second, around(120.0, 90.0));
    REQUIRE(track.usable);
    // Well inside half a pixel: a whole-pixel answer would be off by 0.5 and
    // a mask sitting on it would chatter frame to frame.
    CHECK(track.dx == Approx(3.5).margin(0.25));
    CHECK(track.dy == Approx(2.25).margin(0.25));
}

TEST_CASE("a brightness change between frames does not look like motion", "[tracker]") {
    const render::RgbaImage first = textured(320, 240, 0.0, 0.0);
    render::RgbaImage second = textured(320, 240, 4.0, 0.0);
    for (std::int32_t y = 0; y < second.height(); ++y) {
        for (std::int32_t x = 0; x < second.width(); ++x) {
            render::Rgba& pixel = second.at(x, y);
            // A stop up and a lift, together: the case a difference metric
            // reads as the patch having moved somewhere darker.
            pixel.r = (pixel.r * 2.0F) + 0.1F;
            pixel.g = (pixel.g * 2.0F) + 0.1F;
            pixel.b = (pixel.b * 2.0F) + 0.1F;
        }
    }

    const render::PatchTrack track = render::trackPatch(first, second, around(120.0, 90.0));
    REQUIRE(track.usable);
    CHECK(track.dx == Approx(4.0).margin(0.25));
    CHECK(track.dy == Approx(0.0).margin(0.25));
}

TEST_CASE("a flat patch is refused rather than guessed at", "[tracker]") {
    render::RgbaImage flat{160, 120};
    flat.fill(render::Rgba{0.5F, 0.5F, 0.5F, 1.0F});
    const render::RgbaImage also = flat.clone();

    const render::PatchTrack track = render::trackPatch(flat, also, around(80.0, 60.0));
    CHECK_FALSE(track.usable);
    CHECK_FALSE(track.reason.empty());
}

TEST_CASE("a patch that is not in the next frame is refused", "[tracker]") {
    const render::RgbaImage first = textured(320, 240, 0.0, 0.0);
    render::RgbaImage second{320, 240};
    std::mt19937 noise{99};
    std::uniform_real_distribution<double> grain{0.0, 1.0};
    for (std::int32_t y = 0; y < second.height(); ++y) {
        for (std::int32_t x = 0; x < second.width(); ++x) {
            const auto value = static_cast<float>(grain(noise));
            second.at(x, y) = render::Rgba{value, value, value, 1.0F};
        }
    }

    const render::PatchTrack track = render::trackPatch(first, second, around(120.0, 90.0));
    CHECK_FALSE(track.usable);
    CHECK(track.reason.find("lost it") != std::string::npos);
}

TEST_CASE("a track near the edge of the frame still works", "[tracker]") {
    const render::RgbaImage first = textured(320, 240, 0.0, 0.0);
    const render::RgbaImage second = textured(320, 240, -6.0, 0.0);

    // Close enough to the left edge that most of the search window reads
    // outside the frame. Those offsets are refused rather than clamped -- a
    // clamped read repeats the border pixel, which correlates perfectly with
    // itself and would win every time -- and the offset that is readable is
    // still found.
    render::PatchWindow window = around(40.0, 120.0);
    window.halfWidth = 25.0;
    window.halfHeight = 40.0;
    const render::PatchTrack track = render::trackPatch(first, second, window);
    REQUIRE(track.usable);
    CHECK(track.dx == Approx(-6.0).margin(0.3));
    CHECK(track.dy == Approx(0.0).margin(0.3));
}
