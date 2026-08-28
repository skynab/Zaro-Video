#include <cstdint>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/Compare.h"

using namespace zaro;
using Catch::Approx;

namespace {

render::RgbaImage flat(std::int32_t width, std::int32_t height, float r, float g, float b) {
    render::RgbaImage image{width, height};
    image.fill(render::Rgba{r, g, b, 1.0F});
    return image;
}

}  // namespace

TEST_CASE("A split puts the reference on the left and the current on the right",
          "[render][compare]") {
    const render::RgbaImage reference = flat(64, 16, 1.0F, 0.0F, 0.0F);
    const render::RgbaImage current = flat(64, 16, 0.0F, 0.0F, 1.0F);
    render::RgbaImage out;
    render::compareFrames(reference, current, out, render::CompareMode::Split, 0.5);

    REQUIRE(out.width() == 64);
    CHECK(out.at(10, 8).r == Approx(1.0F));
    CHECK(out.at(10, 8).b == Approx(0.0F));
    CHECK(out.at(50, 8).b == Approx(1.0F));
    CHECK(out.at(50, 8).r == Approx(0.0F));
}

TEST_CASE("The split moves where it is told", "[render][compare]") {
    const render::RgbaImage reference = flat(100, 4, 1.0F, 0.0F, 0.0F);
    const render::RgbaImage current = flat(100, 4, 0.0F, 0.0F, 1.0F);
    render::RgbaImage out;

    render::compareFrames(reference, current, out, render::CompareMode::Split, 0.25);
    CHECK(out.at(10, 2).r == Approx(1.0F));
    CHECK(out.at(40, 2).b == Approx(1.0F));

    render::compareFrames(reference, current, out, render::CompareMode::Split, 0.9);
    CHECK(out.at(40, 2).r == Approx(1.0F));
    CHECK(out.at(95, 2).b == Approx(1.0F));

    SECTION("and all the way over shows one frame whole") {
        render::compareFrames(reference, current, out, render::CompareMode::Split, 0.0);
        CHECK(out.at(2, 2).b == Approx(1.0F));
        render::compareFrames(reference, current, out, render::CompareMode::Split, 1.0);
        CHECK(out.at(97, 2).r == Approx(1.0F));
    }
}

TEST_CASE("A split copies rather than resamples", "[render][compare]") {
    // The point of a split is judging a difference in detail. Resampling one
    // side would invent a difference of its own, so a single-pixel feature has
    // to survive exactly.
    render::RgbaImage reference{32, 4};
    reference.fill(render::Rgba{0.0F, 0.0F, 0.0F, 1.0F});
    reference.at(5, 2) = render::Rgba{1.0F, 1.0F, 1.0F, 1.0F};
    const render::RgbaImage current = flat(32, 4, 0.5F, 0.5F, 0.5F);

    render::RgbaImage out;
    render::compareFrames(reference, current, out, render::CompareMode::Split, 0.5);
    CHECK(out.at(5, 2).r == 1.0F);
    CHECK(out.at(4, 2).r == 0.0F);
    CHECK(out.at(6, 2).r == 0.0F);
}

TEST_CASE("Side by side keeps both shapes", "[render][compare]") {
    // A comparison that stretched one shot would be worse than useless for
    // judging a grade, so both halves are scaled evenly and centred.
    const render::RgbaImage reference = flat(64, 32, 1.0F, 0.0F, 0.0F);
    const render::RgbaImage current = flat(64, 32, 0.0F, 0.0F, 1.0F);
    render::RgbaImage out;
    render::compareFrames(reference, current, out, render::CompareMode::SideBySide, 0.5);

    // Each half holds its own picture at half size, so the middle of each
    // quarter is the colour it should be.
    CHECK(out.at(16, 16).r == Approx(1.0F).margin(0.05F));
    CHECK(out.at(48, 16).b == Approx(1.0F).margin(0.05F));
    // Above and below are empty, because a half-height picture is centred.
    CHECK(out.at(16, 1).a == Approx(0.0F));
}

TEST_CASE("With no reference yet, the current frame is shown whole", "[render][compare]") {
    // Somebody who has not set a reference should see what they were seeing,
    // not half a picture.
    const render::RgbaImage current = flat(32, 8, 0.2F, 0.4F, 0.6F);
    render::RgbaImage out;
    render::compareFrames(render::RgbaImage{}, current, out, render::CompareMode::Split, 0.5);
    CHECK(out.at(4, 4).g == Approx(0.4F));
    CHECK(out.at(28, 4).g == Approx(0.4F));
}

TEST_CASE("The divide is visible", "[render][compare]") {
    // Without it, two similar grades read as one picture with an odd seam and
    // somebody spends a while working out which side they are looking at.
    const render::RgbaImage reference = flat(64, 8, 0.4F, 0.4F, 0.4F);
    const render::RgbaImage current = flat(64, 8, 0.42F, 0.42F, 0.42F);
    render::RgbaImage out;
    render::compareFrames(reference, current, out, render::CompareMode::Split, 0.5);
    CHECK(out.at(32, 4).r == Approx(1.0F));
    CHECK(out.at(31, 4).r == Approx(0.4F));
}
