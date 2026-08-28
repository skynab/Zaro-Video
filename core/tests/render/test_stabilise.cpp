#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/Stabilise.h"

using namespace zaro;
using Catch::Approx;

namespace {

/// A source that generates the same textured picture every frame, moved by a
/// path the test chose. The camera motion is then known exactly, which is what
/// makes "did the stabiliser remove it" a question with an answer.
class ShakySource final : public render::FrameSource {
public:
    ShakySource(std::int32_t width, std::int32_t height, std::vector<double> xs,
                std::vector<double> ys)
        : width_{width}, height_{height}, xs_{std::move(xs)}, ys_{std::move(ys)} {}

    /// From this frame on, the picture is of something else entirely -- which
    /// is what a cut is, rather than a very large move.
    void cutAt(std::size_t frame) { cut_ = frame; }

    Result<const render::RgbaImage*> imageFor(model::MediaRefId,
                                              const time::RationalTime& sourceTime) override {
        const auto index = static_cast<std::size_t>(std::max<std::int64_t>(0, sourceTime.frames()));
        const double shiftX = index < xs_.size() ? xs_[index] : xs_.back();
        const double shiftY = index < ys_.size() ? ys_[index] : ys_.back();
        current_ = render::RgbaImage{width_, height_};
        std::mt19937 grain{11};
        std::uniform_real_distribution<double> speckle{-0.02, 0.02};
        for (std::int32_t y = 0; y < height_; ++y) {
            for (std::int32_t x = 0; x < width_; ++x) {
                const double sx = static_cast<double>(x) - shiftX;
                const double sy = static_cast<double>(y) - shiftY;
                // Two frequencies, so the picture is not periodic at the scale
                // the search window covers.
                const double texture =
                    index >= cut_
                        ? (0.3 * std::sin((sx * 0.07) + 2.0)) + (0.25 * std::cos(sy * 0.61))
                        : (0.3 * std::sin(sx * 0.21)) + (0.2 * std::cos(sy * 0.13)) +
                              (0.15 * std::sin((sx + sy) * 0.05));
                const auto value = static_cast<float>(0.5 + texture + speckle(grain));
                current_.at(x, y) = render::Rgba{value, value, value, 1.0F};
            }
        }
        return &current_;
    }

private:
    std::int32_t width_;
    std::int32_t height_;
    std::vector<double> xs_;
    std::vector<double> ys_;
    std::size_t cut_{~std::size_t{0}};
    render::RgbaImage current_;
};

std::vector<time::RationalTime> frames(int count) {
    std::vector<time::RationalTime> times;
    times.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        times.emplace_back(i, time::rates::fps24);
    }
    return times;
}

/// How far the picture still moves once the correction is applied: the number
/// somebody actually cares about.
double residual(const std::vector<double>& path, const std::vector<double>& correction) {
    double worst = 0.0;
    for (std::size_t i = 1; i < correction.size(); ++i) {
        const double before = path[i] + correction[i];
        const double after = path[i - 1] + correction[i - 1];
        worst = std::max(worst, std::fabs(before - after));
    }
    return worst;
}

}  // namespace

TEST_CASE("shake is taken out of a static shot", "[stabilise]") {
    constexpr int kFrames = 24;
    std::vector<double> shakeX;
    std::vector<double> shakeY;
    for (int i = 0; i < kFrames; ++i) {
        // Four pixels of jitter, alternating: hand shake, not a move.
        shakeX.push_back(4.0 * std::sin(static_cast<double>(i) * 2.1));
        shakeY.push_back(3.0 * std::cos(static_cast<double>(i) * 1.7));
    }
    ShakySource source{320, 240, shakeX, shakeY};

    const auto result = render::stabilise(source, model::MediaRefId{1}, frames(kFrames));
    REQUIRE(result);
    CHECK(result->measured == kFrames - 1);
    CHECK(result->stopped.empty());
    REQUIRE(result->x.size() == static_cast<std::size_t>(kFrames));

    const double before = residual(shakeX, std::vector<double>(shakeX.size(), 0.0));
    const double after = residual(shakeX, result->x);
    CHECK(before > 3.0);
    // Most of the shake gone, not all of it: a moving average leaves a little,
    // and a test that demanded none would be testing the arithmetic of an
    // exact-cancellation that no real shot allows.
    CHECK(after < before / 3.0);
    CHECK(residual(shakeY, result->y) < residual(shakeY, std::vector<double>(shakeY.size(), 0.0)));
    // Zoomed in enough to hide what the corrections expose, and not by much.
    CHECK(result->zoom > 1.0);
    CHECK(result->zoom < 1.2);
}

TEST_CASE("a deliberate pan is followed rather than fought", "[stabilise]") {
    constexpr int kFrames = 24;
    std::vector<double> pan;
    std::vector<double> still;
    for (int i = 0; i < kFrames; ++i) {
        pan.push_back(2.0 * static_cast<double>(i));  // a steady two pixels a frame
        still.push_back(0.0);
    }
    ShakySource source{320, 240, pan, still};

    const auto result = render::stabilise(source, model::MediaRefId{1}, frames(kFrames));
    REQUIRE(result);
    // The corrections stay small and bounded: a stabiliser that treated the pan
    // as shake would have to push the picture back further every frame, and
    // would run out of frame to do it with.
    for (const double correction : result->x) {
        CHECK(std::fabs(correction) < 8.0);
    }
    CHECK(result->zoom < 1.1);
}

TEST_CASE("a cut stops the analysis instead of poisoning it", "[stabilise]") {
    constexpr int kFrames = 16;
    std::vector<double> shakeX(kFrames, 0.0);
    std::vector<double> shakeY(kFrames, 0.0);
    ShakySource source{320, 240, shakeX, shakeY};
    source.cutAt(8);

    const auto result = render::stabilise(source, model::MediaRefId{1}, frames(kFrames));
    REQUIRE(result);
    CHECK(result->measured < kFrames - 1);
    CHECK_FALSE(result->stopped.empty());
}

TEST_CASE("footage with nothing in it is refused, not answered with zero", "[stabilise]") {
    // A flat grey field: every offset correlates with every other, so an
    // answer of "it did not move" would be indistinguishable from a shot that
    // really did not move, and much more misleading.
    class FlatSource final : public render::FrameSource {
    public:
        Result<const render::RgbaImage*> imageFor(model::MediaRefId,
                                                  const time::RationalTime&) override {
            current_ = render::RgbaImage{320, 240};
            current_.fill(render::Rgba{0.5F, 0.5F, 0.5F, 1.0F});
            return &current_;
        }

    private:
        render::RgbaImage current_;
    };

    FlatSource source;
    const auto result = render::stabilise(source, model::MediaRefId{1}, frames(8));
    REQUIRE_FALSE(result);
    CHECK_FALSE(result.error().message().empty());
}

TEST_CASE("too few frames is refused", "[stabilise]") {
    ShakySource source{320, 240, {0.0, 0.0}, {0.0, 0.0}};
    const auto result = render::stabilise(source, model::MediaRefId{1}, frames(2));
    CHECK_FALSE(result);
}
