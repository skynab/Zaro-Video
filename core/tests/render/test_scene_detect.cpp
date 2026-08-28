#include <cstdint>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/render/SceneDetect.h"

#include "ModelFixtures.h"

using namespace zaro;
using Catch::Approx;
using zaro::testing::Fixture;

namespace {

const time::Rational kRate{24, 1};

time::RationalTime at(std::int64_t frame) {
    return time::RationalTime{frame, kRate};
}

render::RgbaImage flat(float r, float g, float b) {
    render::RgbaImage image{32, 32};
    image.fill(render::Rgba{r, g, b, 1.0F});
    return image;
}

/// A picture that moves without changing what is in it: the same colours,
/// shifted. What a pan looks like to a histogram.
render::RgbaImage panned(std::int32_t offset) {
    render::RgbaImage image{32, 32};
    for (std::int32_t y = 0; y < 32; ++y) {
        render::Rgba* row = image.row(y);
        for (std::int32_t x = 0; x < 32; ++x) {
            const bool light = (((x + offset) / 4) % 2) == 0;
            row[x] = light ? render::Rgba{0.6F, 0.5F, 0.4F, 1.0F}
                           : render::Rgba{0.05F, 0.08F, 0.06F, 1.0F};
        }
    }
    return image;
}

/// Two textured shots, mixed. Flat colours would not do here: a flat frame's
/// histogram is a single bin per channel, so the smallest change of value moves
/// all of it and reads as a total change. Real footage has a spread, and a
/// dissolve moves it gradually -- which is the thing being measured.
render::RgbaImage mixedShots(float mix) {
    render::RgbaImage image{32, 32};
    for (std::int32_t y = 0; y < 32; ++y) {
        render::Rgba* row = image.row(y);
        for (std::int32_t x = 0; x < 32; ++x) {
            const auto spread = static_cast<float>(((x * 7) + (y * 3)) % 32) / 32.0F;
            const render::Rgba warm{0.15F + (spread * 0.7F), 0.10F + (spread * 0.35F),
                                    0.05F + (spread * 0.15F), 1.0F};
            const render::Rgba cool{0.05F + (spread * 0.15F), 0.10F + (spread * 0.35F),
                                    0.20F + (spread * 0.7F), 1.0F};
            row[x] =
                render::Rgba{warm.r + ((cool.r - warm.r) * mix), warm.g + ((cool.g - warm.g) * mix),
                             warm.b + ((cool.b - warm.b) * mix), 1.0F};
        }
    }
    return image;
}

render::SceneDetectOptions options(std::int32_t confirmAfter = 3, std::int64_t minimumShot = 12) {
    render::SceneDetectOptions out;
    out.confirmAfter = confirmAfter;
    out.minimumShot = at(minimumShot);
    return out;
}

}  // namespace

TEST_CASE("A cut between two shots is found", "[render][scene]") {
    render::SceneDetector detector{options()};
    for (std::int64_t frame = 0; frame < 20; ++frame) {
        detector.push(flat(0.7F, 0.2F, 0.1F), at(frame));
    }
    for (std::int64_t frame = 20; frame < 40; ++frame) {
        detector.push(flat(0.05F, 0.1F, 0.6F), at(frame));
    }
    detector.flush();

    REQUIRE(detector.cuts().size() == 1);
    CHECK(detector.cuts().front().at.frames() == 20);
    CHECK(detector.cuts().front().confidence > 0.9);
}

TEST_CASE("A flash is not a cut", "[render][scene]") {
    // A camera flash differs enormously from the frame before it and then goes
    // straight back to looking exactly like it. This is the whole reason a
    // candidate has to survive a few frames before it counts.
    render::SceneDetector detector{options()};
    for (std::int64_t frame = 0; frame < 40; ++frame) {
        const bool lit = frame == 20;
        detector.push(lit ? flat(1.0F, 1.0F, 1.0F) : flat(0.2F, 0.3F, 0.25F), at(frame));
    }
    detector.flush();
    CHECK(detector.cuts().empty());

    SECTION("but with confirmation switched off it is reported") {
        // Stated as a test so the difference the check makes is visible: the
        // flash produces two candidates, going in and coming out.
        render::SceneDetector naive{options(0, 1)};
        for (std::int64_t frame = 0; frame < 40; ++frame) {
            const bool lit = frame == 20;
            naive.push(lit ? flat(1.0F, 1.0F, 1.0F) : flat(0.2F, 0.3F, 0.25F), at(frame));
        }
        naive.flush();
        CHECK(naive.cuts().size() == 2);
    }
}

TEST_CASE("A pan is not a cut", "[render][scene]") {
    // Every pixel changes and almost no bin does, which is exactly why the
    // measurement is a histogram rather than a difference of pixels.
    render::SceneDetector detector{options()};
    for (std::int64_t frame = 0; frame < 40; ++frame) {
        detector.push(panned(static_cast<std::int32_t>(frame)), at(frame));
    }
    detector.flush();
    CHECK(detector.cuts().empty());
}

TEST_CASE("A dissolve is not a cut", "[render][scene]") {
    render::SceneDetector detector{options()};
    for (std::int64_t frame = 0; frame < 10; ++frame) {
        detector.push(mixedShots(0.0F), at(frame));
    }
    for (std::int64_t frame = 10; frame < 30; ++frame) {
        detector.push(mixedShots(static_cast<float>(frame - 10) / 19.0F), at(frame));
    }
    for (std::int64_t frame = 30; frame < 50; ++frame) {
        detector.push(mixedShots(1.0F), at(frame));
    }
    detector.flush();
    // A dissolve moves the distribution a little at a time, so no pair of
    // frames crosses the threshold and the whole transition passes unremarked.
    // That is the honest outcome: a dissolve is not a cut, and reporting one
    // in the middle of it would split a shot where nobody made an edit.
    CHECK(detector.cuts().empty());
}

TEST_CASE("Two cuts close together are reported once", "[render][scene]") {
    render::SceneDetector detector{options(1, 12)};
    for (std::int64_t frame = 0; frame < 20; ++frame) {
        detector.push(flat(0.8F, 0.1F, 0.1F), at(frame));
    }
    for (std::int64_t frame = 20; frame < 25; ++frame) {
        detector.push(flat(0.1F, 0.8F, 0.1F), at(frame));
    }
    for (std::int64_t frame = 25; frame < 45; ++frame) {
        detector.push(flat(0.1F, 0.1F, 0.8F), at(frame));
    }
    detector.flush();
    CHECK(detector.cuts().size() == 1);
    CHECK(detector.cuts().front().at.frames() == 20);
}

TEST_CASE("A change right at the start is not a cut", "[render][scene]") {
    // A flash on the head of a take, or a black frame the encoder left there.
    // The clip already begins at frame zero; splitting a frame off the front of
    // it produces a piece nobody asked for, and there is not enough material
    // before it to tell a cut from the shot simply beginning.
    render::SceneDetector detector{options()};
    detector.push(flat(0.0F, 0.0F, 0.0F), at(0));
    for (std::int64_t frame = 1; frame < 40; ++frame) {
        detector.push(flat(0.6F, 0.5F, 0.4F), at(frame));
    }
    detector.flush();
    CHECK(detector.cuts().empty());

    SECTION("but the same change further in is") {
        render::SceneDetector later{options()};
        for (std::int64_t frame = 0; frame < 20; ++frame) {
            later.push(flat(0.0F, 0.0F, 0.0F), at(frame));
        }
        for (std::int64_t frame = 20; frame < 40; ++frame) {
            later.push(flat(0.6F, 0.5F, 0.4F), at(frame));
        }
        later.flush();
        REQUIRE(later.cuts().size() == 1);
        CHECK(later.cuts().front().at.frames() == 20);
    }
}

TEST_CASE("A cut in the last frames is still a cut", "[render][scene]") {
    // Nothing after it to confirm against; the evidence is the same as for any
    // other, and only the confirmation is missing.
    render::SceneDetector detector{options()};
    for (std::int64_t frame = 0; frame < 20; ++frame) {
        detector.push(flat(0.7F, 0.2F, 0.1F), at(frame));
    }
    detector.push(flat(0.05F, 0.1F, 0.6F), at(20));
    CHECK(detector.cuts().empty());
    detector.flush();
    CHECK(detector.cuts().size() == 1);
}

TEST_CASE("A histogram is a distribution, not a picture", "[render][scene]") {
    std::array<double, 48> a{};
    std::array<double, 48> b{};
    render::sceneHistogram(flat(0.5F, 0.5F, 0.5F), a);
    render::sceneHistogram(flat(0.5F, 0.5F, 0.5F), b);
    CHECK(render::histogramDistance(a, b) == Approx(0.0));

    render::sceneHistogram(flat(0.0F, 0.0F, 0.0F), b);
    CHECK(render::histogramDistance(a, b) == Approx(1.0));

    SECTION("and a shifted picture has the same one") {
        render::sceneHistogram(panned(0), a);
        render::sceneHistogram(panned(8), b);
        CHECK(render::histogramDistance(a, b) < 0.05);
    }
}

TEST_CASE("Cutting at several points is one undoable step", "[edit][scene]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 100))));
    REQUIRE(f.track(f.v1).clips().size() == 1);

    REQUIRE(f.run(edit::makeRazorAt(f.project, f.on(f.v1), {f.at(25), f.at(50), f.at(75)})));
    CHECK(f.track(f.v1).clips().size() == 4);

    f.stack.undo(f.project);
    // The clip somebody had, not the cuts peeled off one at a time in an order
    // they never chose.
    CHECK(f.track(f.v1).clips().size() == 1);
}

TEST_CASE("Points that land nowhere are skipped, not refused", "[edit][scene]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));

    // One inside, one past the end, one on the existing cut at the start.
    REQUIRE(f.run(edit::makeRazorAt(f.project, f.on(f.v1), {f.at(20), f.at(400), f.at(0)})));
    CHECK(f.track(f.v1).clips().size() == 2);

    SECTION("and a list with nothing usable in it is refused") {
        CHECK_FALSE(edit::makeRazorAt(f.project, f.on(f.v1), {f.at(900), f.at(950)}));
    }
}

TEST_CASE("The same point twice does not make a clip of no length", "[edit][scene]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 60))));
    REQUIRE(f.run(edit::makeRazorAt(f.project, f.on(f.v1), {f.at(30), f.at(30)})));
    CHECK(f.track(f.v1).clips().size() == 2);
    for (const model::Clip& clip : f.track(f.v1).clips()) {
        CHECK(clip.timelineRange.duration().frames() > 0);
    }
}
