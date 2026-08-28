#include <cstdint>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"
#include "zaro/core/render/AudioGraph.h"
#include "zaro/core/render/RenderGraph.h"

#include "ModelFixtures.h"
#include "TestSources.h"

using namespace zaro;
using Catch::Approx;
using zaro::testing::ConstantAudioSource;
using zaro::testing::Fixture;
using zaro::testing::SolidFrameSource;

namespace {

/// A fixture with one 100-frame clip reading a ramp, so a test can say which
/// source frame reached the screen rather than only that one did.
struct RemapFixture : Fixture {
    SolidFrameSource source{8, 8};
    render::RenderGraph graph{source};
    model::ClipId clipId;

    RemapFixture() {
        sequence().setSize(8, 8);
        source.defineRamp(longMedia);
        // Source starts at 0 so that "source second" and "frame / 25" agree,
        // which keeps the expectations in this file readable.
        model::Clip c = clip(0, 100, 0);
        clipId = c.id;
        REQUIRE(run(edit::makeOverwrite(project, on(v1), c)));
    }

    [[nodiscard]] const model::Clip& live() const { return *track(v1).find(clipId); }

    /// The source frame the renderer actually fetched for a timeline frame.
    [[nodiscard]] std::int64_t fetchedAt(std::int64_t frame) {
        const std::size_t before = source.requests().size();
        REQUIRE(graph.composite(sequence(), at(frame)));
        REQUIRE(source.requests().size() > before);
        return source.requests().back().frames();
    }

    void setRemap(const std::vector<std::pair<std::int64_t, double>>& points,
                  model::Interpolation interpolation = model::Interpolation::Linear) {
        model::Clip& target = const_cast<model::Clip&>(live());
        model::Curve& curve = target.animation.curve(model::Param::TimeRemap);
        curve = model::Curve{};
        for (const auto& [frame, seconds] : points) {
            model::Keyframe key;
            key.time = at(frame);
            key.value = seconds;
            key.interpolation = interpolation;
            curve.set(key);
        }
    }
};

}  // namespace

TEST_CASE("Turning time remapping on does not change the picture", "[edit][remap]") {
    // The whole point of seeding the identity: switching it on is a statement
    // about what can be edited next, not an edit.
    RemapFixture f;
    std::vector<std::int64_t> before;
    for (std::int64_t frame = 0; frame < 100; frame += 7) {
        before.push_back(f.fetchedAt(frame));
    }

    REQUIRE(f.run(edit::makeSetTimeRemapped(f.project, f.on(f.v1), f.clipId, true)));
    REQUIRE(f.live().isTimeRemapped());

    std::vector<std::int64_t> after;
    for (std::int64_t frame = 0; frame < 100; frame += 7) {
        after.push_back(f.fetchedAt(frame));
    }
    CHECK(after == before);
}

TEST_CASE("A shallower curve is a slower clip", "[edit][remap]") {
    RemapFixture f;
    // Four seconds of timeline showing two seconds of source: half speed.
    f.setRemap({{0, 0.0}, {99, 2.0}});

    CHECK(f.fetchedAt(0) == 0);
    CHECK(f.fetchedAt(50) == 25);
    CHECK(f.fetchedAt(99) == 50);
}

TEST_CASE("A falling curve runs the picture backwards", "[edit][remap]") {
    RemapFixture f;
    f.setRemap({{0, 2.0}, {99, 0.0}});

    CHECK(f.fetchedAt(0) == 50);
    CHECK(f.fetchedAt(99) == 0);
    // And it passes through the middle on the way, rather than jumping.
    CHECK(f.fetchedAt(50) == 25);
}

TEST_CASE("A held curve is a freeze", "[edit][remap]") {
    RemapFixture f;
    f.setRemap({{0, 1.0}, {99, 1.0}}, model::Interpolation::Hold);

    CHECK(f.fetchedAt(0) == 25);
    CHECK(f.fetchedAt(40) == 25);
    CHECK(f.fetchedAt(99) == 25);
}

TEST_CASE("A curve that runs off the front of the file stops at the first frame", "[edit][remap]") {
    RemapFixture f;
    f.setRemap({{0, -5.0}, {99, 1.0}});
    // Not a negative source time, and not a clip that quietly stops drawing.
    CHECK(f.fetchedAt(0) == 0);
    CHECK(f.fetchedAt(99) == 25);
}

TEST_CASE("Freezing holds the frame that was showing", "[edit][remap]") {
    RemapFixture f;
    const std::int64_t showing = f.fetchedAt(60);
    REQUIRE(showing == 60);

    REQUIRE(f.run(edit::makeFreezeFrame(f.project, f.on(f.v1), f.clipId, f.at(60))));
    CHECK(f.fetchedAt(0) == showing);
    CHECK(f.fetchedAt(60) == showing);
    CHECK(f.fetchedAt(99) == showing);

    SECTION("and undo gives the clip back") {
        f.stack.undo(f.project);
        CHECK_FALSE(f.live().isTimeRemapped());
        CHECK(f.fetchedAt(0) == 0);
        CHECK(f.fetchedAt(99) == 99);
    }

    SECTION("and freezing outside the clip is refused") {
        CHECK_FALSE(edit::makeFreezeFrame(f.project, f.on(f.v1), f.clipId, f.at(500)));
    }
}

TEST_CASE("Freezing a clip that is already remapped freezes what is on screen", "[edit][remap]") {
    RemapFixture f;
    f.setRemap({{0, 0.0}, {99, 2.0}});
    const std::int64_t showing = f.fetchedAt(50);
    REQUIRE(showing == 25);

    REQUIRE(f.run(edit::makeFreezeFrame(f.project, f.on(f.v1), f.clipId, f.at(50))));
    CHECK(f.fetchedAt(10) == showing);
    CHECK(f.fetchedAt(90) == showing);
}

TEST_CASE("Removing the remap puts the clip back on its ranges", "[edit][remap]") {
    RemapFixture f;
    REQUIRE(f.run(edit::makeSetTimeRemapped(f.project, f.on(f.v1), f.clipId, true)));
    f.setRemap({{0, 1.0}, {99, 1.0}}, model::Interpolation::Hold);
    REQUIRE(f.fetchedAt(80) == 25);

    REQUIRE(f.run(edit::makeSetTimeRemapped(f.project, f.on(f.v1), f.clipId, false)));
    CHECK_FALSE(f.live().isTimeRemapped());
    CHECK(f.fetchedAt(80) == 80);

    SECTION("and asking twice is refused rather than silently doing nothing") {
        CHECK_FALSE(edit::makeSetTimeRemapped(f.project, f.on(f.v1), f.clipId, false));
    }
}

TEST_CASE("Other keyframes run in time, not in retimed picture time", "[edit][remap]") {
    // A fade drawn across a freeze still fades. Anything else would mean
    // stopping the picture stopped the graphics with it, which is not what
    // dragging an opacity ramp over a frozen shot means.
    RemapFixture f;
    f.setRemap({{0, 1.0}, {99, 1.0}}, model::Interpolation::Hold);

    model::Clip& target = const_cast<model::Clip&>(f.live());
    model::Curve& opacity = target.animation.curve(model::Param::Opacity);
    opacity.set(model::Keyframe{f.at(0), 0.0, model::Interpolation::Linear, {}, {}});
    opacity.set(model::Keyframe{f.at(99), 1.0, model::Interpolation::Linear, {}, {}});

    CHECK(f.live().transformAt(f.at(0)).opacity == Approx(0.0));
    CHECK(f.live().transformAt(f.at(99)).opacity == Approx(1.0).margin(0.01));
    // Halfway through the freeze, halfway through the fade.
    CHECK(f.live().transformAt(f.at(50)).opacity == Approx(0.5).margin(0.02));
}

TEST_CASE("Time remapping leaves the sound alone", "[edit][remap]") {
    // Retiming a signal is resampling it, and a remap changes rate
    // continuously. Rather than do that badly in passing, the sound runs at the
    // clip's own speed and the picture is what moves.
    RemapFixture f;
    const model::Clip& video = f.live();

    const time::RationalTime freezeAt = f.at(50);
    const time::RationalTime soundBefore = video.activeBaseSourceTimeAt(freezeAt);
    f.setRemap({{0, 0.0}, {99, 0.0}}, model::Interpolation::Hold);

    CHECK(f.live().activeBaseSourceTimeAt(freezeAt) == soundBefore);
    // While the picture has stopped dead.
    CHECK(f.live().activeSourceTimeAt(freezeAt).frames() == 0);
}

TEST_CASE("A time remap survives a round trip through a project file", "[edit][remap][io]") {
    RemapFixture f;
    REQUIRE(f.run(edit::makeFreezeFrame(f.project, f.on(f.v1), f.clipId, f.at(60))));

    auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    auto reloaded = io::loadProjectFromString(*text);
    REQUIRE(reloaded);

    const model::Clip& back =
        reloaded->project.findSequence(f.sequenceId)->findTrack(f.v1)->clips().front();
    REQUIRE(back.isTimeRemapped());
    CHECK(back.animation.find(model::Param::TimeRemap)->size() == 2);
    CHECK(back.sourceTimeAt(f.at(10)) == f.live().sourceTimeAt(f.at(10)));
}
