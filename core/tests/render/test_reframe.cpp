#include <algorithm>
#include <cmath>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/Reframe.h"

using namespace zaro;
using Catch::Approx;

namespace {

/// A wide frame that is flat everywhere except for one patch of detail, whose
/// centre the test chooses. Reframing has one right answer against this.
class SubjectSource final : public render::FrameSource {
public:
    SubjectSource(std::int32_t width, std::int32_t height, std::vector<double> centres)
        : width_{width}, height_{height}, centres_{std::move(centres)} {}

    Result<const render::RgbaImage*> imageFor(model::MediaRefId,
                                              const time::RationalTime& sourceTime) override {
        const auto index = static_cast<std::size_t>(std::max<std::int64_t>(0, sourceTime.frames()));
        const double centre = index < centres_.size() ? centres_[index] : centres_.back();
        current_ = render::RgbaImage{width_, height_};
        current_.fill(render::Rgba{0.2F, 0.2F, 0.2F, 1.0F});
        if (centre < 0.0) {
            return &current_;  // nothing in it at all
        }
        // A block of fine stripes: detail, not merely brightness, since what
        // is measured is edge energy.
        const auto from = static_cast<std::int32_t>(std::max(0.0, centre - 40.0));
        const auto to = static_cast<std::int32_t>(std::min<double>(width_ - 1, centre + 40.0));
        for (std::int32_t y = height_ / 3; y < (2 * height_) / 3; ++y) {
            for (std::int32_t x = from; x <= to; ++x) {
                const float value = (x % 4 < 2) ? 0.9F : 0.1F;
                current_.at(x, y) = render::Rgba{value, value, value, 1.0F};
            }
        }
        return &current_;
    }

private:
    std::int32_t width_;
    std::int32_t height_;
    std::vector<double> centres_;
    render::RgbaImage current_;
};

std::vector<time::RationalTime> frames(int count) {
    std::vector<time::RationalTime> times;
    for (int i = 0; i < count; ++i) {
        times.emplace_back(i, time::rates::fps25);
    }
    return times;
}

}  // namespace

TEST_CASE("a portrait frame is filled, with no empty edge", "[reframe]") {
    SubjectSource source{1920, 1080, std::vector<double>(10, 960.0)};
    const auto result = render::autoReframe(source, model::MediaRefId{1}, frames(10), 1080, 1920);
    REQUIRE(result);
    // 1920 tall from 1080: the picture has to grow by 16/9 to cover.
    CHECK(result->scale == Approx(1920.0 / 1080.0).margin(0.001));
    CHECK(result->measured == 10);
}

TEST_CASE("the frame goes where the detail is", "[reframe]") {
    // The subject sits a quarter of the way across a 1920-wide frame.
    SubjectSource source{1920, 1080, std::vector<double>(12, 480.0)};
    const auto result = render::autoReframe(source, model::MediaRefId{1}, frames(12), 1080, 1920);
    REQUIRE(result);
    REQUIRE(result->x.size() == 12);

    // Moving the picture right by (960 - 480) source pixels, scaled up, is what
    // puts a subject on the left of the frame into the middle of a tall one.
    const double wanted = (960.0 - 480.0) * result->scale;
    CHECK(result->x.back() == Approx(wanted).margin(60.0));
    CHECK(result->x.back() > 0.0);
}

TEST_CASE("the frame follows a subject that moves, without overshooting it", "[reframe]") {
    std::vector<double> walking;
    for (int i = 0; i < 25; ++i) {
        walking.push_back(300.0 + (40.0 * static_cast<double>(i)));  // left to right
    }
    SubjectSource source{1920, 1080, walking};
    const auto result = render::autoReframe(source, model::MediaRefId{1}, frames(25), 1080, 1920);
    REQUIRE(result);

    // It ends further left than it started -- the picture is pushed the other
    // way as the subject walks right.
    CHECK(result->x.front() > result->x.back());
    // A steady walk is followed at the walk's own speed. Smoothing removes
    // jitter, not motion: the average of a ramp is the same ramp, which is
    // what makes a pan come out as a pan rather than as a lag.
    for (std::size_t i = 1; i < result->x.size(); ++i) {
        CHECK(std::fabs(result->x[i] - result->x[i - 1]) <= (40.0 * result->scale) + 0.5);
    }
}

TEST_CASE("a subject that jitters does not make the frame jitter", "[reframe]") {
    // Something flicking back and forth between two places, which is what a
    // per-frame decision would chase and a camera operator would not.
    std::vector<double> flicking;
    for (int i = 0; i < 25; ++i) {
        flicking.push_back(i % 2 == 0 ? 700.0 : 1220.0);
    }
    SubjectSource source{1920, 1080, flicking};
    const auto result = render::autoReframe(source, model::MediaRefId{1}, frames(25), 1080, 1920);
    REQUIRE(result);

    double worst = 0.0;
    for (std::size_t i = 1; i < result->x.size(); ++i) {
        worst = std::max(worst, std::fabs(result->x[i] - result->x[i - 1]));
    }
    // The subject moves 520 source pixels every frame; the frame must not.
    CHECK(worst < 0.2 * 520.0 * result->scale);
}

TEST_CASE("a shot with nothing in it is centred, and says so", "[reframe]") {
    SubjectSource source{1920, 1080, std::vector<double>(8, -1.0)};
    const auto result = render::autoReframe(source, model::MediaRefId{1}, frames(8), 1080, 1920);
    REQUIRE(result);
    for (const double x : result->x) {
        CHECK(x == Approx(0.0).margin(1.0));
    }
    CHECK_FALSE(result->reason.empty());
}

TEST_CASE("a frame that already fits is left where it is", "[reframe]") {
    SubjectSource source{1920, 1080, std::vector<double>(6, 480.0)};
    // Same shape as the source: there is no slack to move in.
    const auto result = render::autoReframe(source, model::MediaRefId{1}, frames(6), 1920, 1080);
    REQUIRE(result);
    CHECK(result->scale == Approx(1.0).margin(0.001));
    for (const double x : result->x) {
        CHECK(x == Approx(0.0).margin(1.0));
    }
}

TEST_CASE("reframing needs something to reframe", "[reframe]") {
    SubjectSource source{1920, 1080, {480.0}};
    CHECK_FALSE(render::autoReframe(source, model::MediaRefId{1}, {}, 1080, 1920));
    CHECK_FALSE(render::autoReframe(source, model::MediaRefId{1}, frames(4), 0, 1920));
}
