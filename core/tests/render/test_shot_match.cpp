#include <cstdint>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/Grade.h"
#include "zaro/core/render/ShotMatch.h"

using namespace zaro;
using Catch::Approx;

namespace {

/// A frame with a spread of values, so it has anchors to match on.
render::RgbaImage ramp(std::int32_t width, std::int32_t height, float low, float high,
                       float redGain = 1.0F) {
    render::RgbaImage image{width, height};
    for (std::int32_t y = 0; y < height; ++y) {
        render::Rgba* row = image.row(y);
        for (std::int32_t x = 0; x < width; ++x) {
            const float t = static_cast<float>(x) / static_cast<float>(width - 1);
            const float value = low + ((high - low) * t);
            row[x] = render::Rgba{value * redGain, value, value, 1.0F};
        }
    }
    return image;
}

/// Push a frame through a CDL the way gradePixel would.
render::RgbaImage through(const render::RgbaImage& source, const model::ColorWheels& wheels) {
    render::RgbaImage out = source.clone();
    const render::GradeConstants grade =
        render::gradeConstantsFor(model::ColorCorrection{}, wheels);
    for (std::int32_t y = 0; y < out.height(); ++y) {
        render::Rgba* row = out.row(y);
        for (std::int32_t x = 0; x < out.width(); ++x) {
            render::gradePixel(grade, row[x].r, row[x].g, row[x].b, nullptr, nullptr, nullptr,
                               1.0F);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("Matching a shot to itself changes nothing", "[render][shotmatch]") {
    const render::RgbaImage shot = ramp(64, 16, 0.05F, 0.8F);
    auto match = render::matchShot(shot, shot);
    REQUIRE(match);
    CHECK(match->usable);
    CHECK(match->before == Approx(0.0).margin(1e-4));
    CHECK(match->wheels.slopeR == Approx(1.0).margin(0.01));
    CHECK(match->wheels.offsetG == Approx(0.0).margin(0.01));
    CHECK(match->wheels.powerB == Approx(1.0).margin(0.05));
}

TEST_CASE("A shot graded away from another can be matched back", "[render][shotmatch]") {
    // The case this exists for: the same material, moved. The correction should
    // put the anchors back where they came from.
    const render::RgbaImage reference = ramp(96, 24, 0.05F, 0.8F);

    model::ColorWheels drift;
    drift.slopeR = 1.3;
    drift.slopeG = 0.9;
    drift.slopeB = 0.8;
    drift.offsetG = 0.04;
    const render::RgbaImage target = through(reference, drift);

    auto match = render::matchShot(reference, target);
    REQUIRE(match);
    INFO("before " << match->before << ", after " << match->after);
    CHECK(match->usable);
    // Closer than it was, and close enough to be a match rather than a nudge.
    CHECK(match->after < match->before);
    CHECK(match->after < 0.01);
}

TEST_CASE("The match is reported, not just applied", "[render][shotmatch]") {
    // "How much did this move" is the question somebody actually has, so both
    // distances come back rather than a verdict.
    const render::RgbaImage reference = ramp(64, 16, 0.05F, 0.8F);
    model::ColorWheels drift;
    drift.slopeB = 0.6;
    const render::RgbaImage target = through(reference, drift);

    auto match = render::matchShot(reference, target);
    REQUIRE(match);
    CHECK(match->before > 0.01);
    CHECK(match->after < match->before);
}

TEST_CASE("Two shots that are not of the same thing are refused", "[render][shotmatch]") {
    // A confident answer to a question nobody asked is worse than no answer.
    // A nearly-black frame against a nearly-white one needs a slope no
    // colourist would dial.
    const render::RgbaImage dark = ramp(64, 16, 0.001F, 0.01F);
    const render::RgbaImage bright = ramp(64, 16, 0.6F, 0.95F);

    auto match = render::matchShot(bright, dark);
    REQUIRE(match);
    CHECK_FALSE(match->usable);
    CHECK_FALSE(match->reason.empty());
    // The numbers still come back: somebody who wants it anyway can have it.
    CHECK(match->wheels.slopeR != 1.0);
}

TEST_CASE("A channel with no range in it is left alone", "[render][shotmatch]") {
    // A blown-out or solid frame has nothing to stretch, and dividing by that
    // nothing is how a match turns into a NaN.
    render::RgbaImage flat{32, 8};
    flat.fill(render::Rgba{0.5F, 0.5F, 0.5F, 1.0F});
    const render::RgbaImage reference = ramp(32, 8, 0.1F, 0.7F);

    auto match = render::matchShot(reference, flat);
    REQUIRE(match);
    CHECK(std::isfinite(match->wheels.slopeR));
    CHECK(match->wheels.slopeR == 1.0);
    CHECK(match->wheels.offsetR == 0.0);
}

TEST_CASE("Transparent pixels do not drag the shadows down", "[render][shotmatch]") {
    // A transparent pixel is not black, and counting it as one would put every
    // shadow anchor on zero.
    render::RgbaImage withHole = ramp(64, 16, 0.2F, 0.8F);
    for (std::int32_t y = 0; y < 8; ++y) {
        for (std::int32_t x = 0; x < 32; ++x) {
            withHole.at(x, y) = render::Rgba{0.0F, 0.0F, 0.0F, 0.0F};
        }
    }
    const render::RgbaImage solid = ramp(64, 16, 0.2F, 0.8F);

    auto match = render::matchShot(solid, withHole);
    REQUIRE(match);
    CHECK(match->usable);
    // The hole is ignored, so the two read as the same shot.
    CHECK(match->before < 0.05);
}

TEST_CASE("Matching needs two frames", "[render][shotmatch]") {
    const render::RgbaImage shot = ramp(16, 4, 0.1F, 0.9F);
    CHECK_FALSE(render::matchShot(render::RgbaImage{}, shot));
    CHECK_FALSE(render::matchShot(shot, render::RgbaImage{}));
}

TEST_CASE("Two flat frames have nothing to match on", "[render][shotmatch]") {
    // They may be miles apart and there is still nothing here to match *on*: a
    // correction built from three anchors needs three distinguishable anchors.
    // Reporting a successful match of zero channels is the confidently-wrong
    // answer this whole design exists to avoid.
    render::RgbaImage dim{32, 8};
    dim.fill(render::Rgba{0.5F, 0.5F, 0.5F, 1.0F});
    render::RgbaImage bright{32, 8};
    bright.fill(render::Rgba{1.0F, 1.0F, 1.0F, 1.0F});

    auto match = render::matchShot(dim, bright);
    REQUIRE(match);
    CHECK(match->before > 0.4);
    CHECK_FALSE(match->usable);
    CHECK(match->reason.find("range") != std::string::npos);
    // And it proposes nothing rather than something that does nothing.
    CHECK(match->wheels.isIdentity());
}
