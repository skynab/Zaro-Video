#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/ui/TimelineLayout.h"

#include "ModelFixtures.h"

using namespace zaro;
using Catch::Approx;
using ui::TimelineLayout;

namespace {

/// A layout over a 25fps sequence, 1000 pixels wide, 100 pixels per second --
/// so one second is 100px and one frame is 4px.
TimelineLayout makeLayout() {
    TimelineLayout::Metrics metrics;
    metrics.pixelsPerSecond = 100.0;
    metrics.headerWidth = 150;
    metrics.rulerHeight = 26;
    metrics.videoTrackHeight = 60;
    metrics.audioTrackHeight = 50;
    metrics.trackGap = 0;
    metrics.edgeGrabPixels = 6;

    TimelineLayout layout{metrics};
    layout.setViewportSize(1000, 600);
    return layout;
}

const time::Rational kRate = time::rates::fps25;

time::RationalTime at(std::int64_t frames) {
    return time::RationalTime{frames, kRate};
}

}  // namespace

TEST_CASE("Time maps to pixels through the header offset", "[ui][timeline]") {
    const TimelineLayout layout = makeLayout();

    CHECK(layout.xForTime(at(0)) == Approx(150.0));
    CHECK(layout.xForTime(at(25)) == Approx(250.0));  // one second in
    CHECK(layout.xForTime(at(1)) == Approx(154.0));   // one frame is 4px

    SECTION("and back again") {
        CHECK(layout.timeForX(150.0, kRate) == at(0));
        CHECK(layout.timeForX(250.0, kRate) == at(25));
        CHECK(layout.timeForX(154.0, kRate) == at(1));
    }

    SECTION("x inside the headers clamps to the start rather than going negative") {
        CHECK(layout.timeForX(0.0, kRate) == at(0));
        CHECK(layout.timeForX(-500.0, kRate) == at(0));
    }
}

TEST_CASE("Scrolling shifts the mapping", "[ui][timeline]") {
    TimelineLayout layout = makeLayout();
    layout.setScroll(at(50));  // two seconds in

    CHECK(layout.xForTime(at(50)) == Approx(150.0));
    CHECK(layout.xForTime(at(75)) == Approx(250.0));
    CHECK(layout.timeForX(150.0, kRate) == at(50));

    SECTION("scrolling before zero is refused") {
        layout.setScroll(at(-100));
        CHECK(layout.scroll().frames() == 0);
    }
}

TEST_CASE("Zoom keeps the time under the pointer in place", "[ui][timeline]") {
    // The property that makes zooming feel like pulling the timeline rather
    // than watching it jump sideways.
    TimelineLayout layout = makeLayout();
    layout.setScroll(at(100));

    for (const double anchorX : {200.0, 500.0, 900.0}) {
        for (const double factor : {2.0, 0.5, 1.25, 0.8}) {
            TimelineLayout copy = layout;
            const time::RationalTime before = copy.timeForX(anchorX, kRate);
            copy.zoomBy(factor, anchorX, kRate);
            const time::RationalTime after = copy.timeForX(anchorX, kRate);

            INFO("anchor " << anchorX << " factor " << factor << ": " << before.frames() << " -> "
                           << after.frames());
            if (copy.scroll().frames() == 0) {
                // Zooming out near the start cannot hold the anchor without
                // scrolling before zero, which is refused. The anchor moves
                // later instead, and never earlier.
                CHECK(after >= before);
            } else {
                // Within a frame: the scroll position is itself quantised to
                // frames, so exactness beyond that is not available.
                CHECK(std::abs(after.frames() - before.frames()) <= 1);
            }
        }
    }
}

TEST_CASE("Zoom is bounded", "[ui][timeline]") {
    TimelineLayout layout = makeLayout();

    for (int i = 0; i < 200; ++i) {
        layout.zoomBy(2.0, 500.0, kRate);
    }
    CHECK(layout.metrics().pixelsPerSecond <= 40000.0);
    CHECK(layout.metrics().pixelsPerSecond > 0.0);

    for (int i = 0; i < 400; ++i) {
        layout.zoomBy(0.5, 500.0, kRate);
    }
    // Never collapses to zero, which would make every mapping a division by it.
    CHECK(layout.metrics().pixelsPerSecond >= 0.05);
}

TEST_CASE("Zoom to fit puts the whole sequence on screen", "[ui][timeline]") {
    TimelineLayout layout = makeLayout();
    layout.zoomToFit(at(25 * 60));  // one minute

    CHECK(layout.scroll().frames() == 0);
    const double endX = layout.xForTime(at(25 * 60));
    // Inside the viewport, with a little room to spare.
    CHECK(endX <= 1000.0);
    CHECK(endX > 900.0);
}

TEST_CASE("The visible range is what painting has to cover", "[ui][timeline]") {
    // Culling to this is what keeps a four-hour sequence as cheap to draw as a
    // four-minute one.
    TimelineLayout layout = makeLayout();
    layout.setScroll(at(50));

    const time::TimeRange visible = layout.visibleRange(kRate);
    CHECK(visible.start() == at(50));
    // 850 content pixels at 100px/s is 8.5 seconds, which is 212.5 frames --
    // rounded up, because a half-visible frame still has to be painted.
    CHECK(visible.duration().frames() == 213);

    SECTION("a zero-width viewport still yields a usable range") {
        layout.setViewportSize(0, 600);
        CHECK(layout.visibleRange(kRate).duration().frames() >= 1);
    }
}

TEST_CASE("Video stacks upward, audio downward, V1 above A1", "[ui][timeline]") {
    testing::Fixture f;  // V1, V2, A1
    TimelineLayout layout = makeLayout();

    const auto rows = layout.rows(f.sequence());
    REQUIRE(rows.size() == 3);

    const auto find = [&rows](model::TrackId id) {
        return *std::find_if(rows.begin(), rows.end(),
                             [id](const TimelineLayout::Row& r) { return r.track == id; });
    };
    const auto v1 = find(f.v1);
    const auto v2 = find(f.v2);
    const auto a1 = find(f.a1);

    // Higher video tracks composite over lower ones and are drawn above them.
    CHECK(v2.top < v1.top);
    // V1 sits directly above A1.
    CHECK(v1.top < a1.top);
    CHECK(v1.top + v1.height == a1.top);
    // The ruler is above everything.
    CHECK(v2.top == 26);

    CHECK(v1.kind == model::TrackKind::Video);
    CHECK(a1.kind == model::TrackKind::Audio);
    CHECK(v1.index == 0);
    CHECK(v2.index == 1);
}

TEST_CASE("A row given a height of its own grows downward", "[ui][timeline]") {
    testing::Fixture f;  // V1, V2, A1
    TimelineLayout layout = makeLayout();

    const auto find = [](const std::vector<TimelineLayout::Row>& rows, model::TrackId id) {
        return *std::find_if(rows.begin(), rows.end(),
                             [id](const TimelineLayout::Row& r) { return r.track == id; });
    };
    const auto before = layout.rows(f.sequence());

    // A row's top edge is fixed by whatever is above it, so a taller row takes
    // the space below: its own bottom edge moves, and everything under it moves
    // with it. That is what makes dragging the edge of a header feel like
    // dragging that edge rather than rearranging the panel.
    layout.setTrackHeight(f.v2, 120);
    const auto after = layout.rows(f.sequence());
    CHECK(find(after, f.v2).height == 120);
    CHECK(find(after, f.v2).top == find(before, f.v2).top);
    CHECK(find(after, f.v1).top == find(before, f.v1).top + 60);
    CHECK(find(after, f.a1).top == find(before, f.a1).top + 60);
    // The rows stay in contact: no gap opens up where one used to end.
    CHECK(find(after, f.v2).top + 120 == find(after, f.v1).top);

    // The same for V1, which sits directly above A1: the sound block moves down
    // by exactly what V1 gained, and the rows above V1 do not move at all.
    layout.setTrackHeight(f.v1, 90);
    const auto taller = layout.rows(f.sequence());
    const auto v1 = find(taller, f.v1);
    const auto a1 = find(taller, f.a1);
    CHECK(v1.height == 90);
    CHECK(v1.top == find(after, f.v1).top);
    CHECK(find(taller, f.v2).top == find(after, f.v2).top);
    CHECK(v1.top + v1.height == a1.top);
    CHECK(a1.top == find(after, f.a1).top + 30);

    // The point-to-row lookup follows, since it reads the same rows.
    REQUIRE(layout.rowAt(f.sequence(), v1.top + 1).has_value());
    CHECK(layout.rowAt(f.sequence(), v1.top + 1)->track == f.v1);
    CHECK(layout.rowAt(f.sequence(), a1.top + 1)->track == f.a1);

    // Content height counts what each row actually takes rather than a multiple
    // of one kind's default: 26 ruler + 120 + 90 + 50.
    CHECK(layout.contentHeight(f.sequence()) == 26 + 120 + 90 + 50);

    // Cleared, every row goes back to its kind's height.
    layout.clearTrackHeights();
    CHECK(layout.contentHeight(f.sequence()) == 26 + 60 + 60 + 50);
    CHECK(find(layout.rows(f.sequence()), f.v2).height == 60);
    CHECK(find(layout.rows(f.sequence()), f.v1).top == find(before, f.v1).top);
}

TEST_CASE("A point resolves to the row it is in", "[ui][timeline]") {
    testing::Fixture f;
    TimelineLayout layout = makeLayout();

    CHECK_FALSE(layout.rowAt(f.sequence(), 5).has_value());  // in the ruler
    REQUIRE(layout.rowAt(f.sequence(), 30).has_value());
    CHECK(layout.rowAt(f.sequence(), 30)->track == f.v2);
    CHECK(layout.rowAt(f.sequence(), 100)->track == f.v1);
    CHECK(layout.rowAt(f.sequence(), 160)->track == f.a1);
    CHECK_FALSE(layout.rowAt(f.sequence(), 5000).has_value());
}

TEST_CASE("Content height covers the ruler and every track", "[ui][timeline]") {
    testing::Fixture f;
    const TimelineLayout layout = makeLayout();
    // 26 ruler + two video at 60 + one audio at 50.
    CHECK(layout.contentHeight(f.sequence()) == 26 + 120 + 50);
}

TEST_CASE("Hit testing finds clips and distinguishes their edges", "[ui][timeline][hit]") {
    testing::Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(25, 50))));  // 1s to 3s
    TimelineLayout layout = makeLayout();

    const model::ClipId id = f.track(f.v1).clips()[0].id;
    const std::int32_t v1Y = 100;
    const double startX = layout.xForTime(at(25));  // 250
    const double endX = layout.xForTime(at(75));    // 450

    SECTION("the body") {
        const auto hit = layout.hitTest(f.sequence(), 350, v1Y);
        REQUIRE(hit.has_value());
        CHECK(hit->clip == id);
        CHECK(hit->track == f.v1);
        CHECK(hit->part == TimelineLayout::Part::Body);
    }

    SECTION("the in edge") {
        const auto hit = layout.hitTest(f.sequence(), static_cast<std::int32_t>(startX) + 2, v1Y);
        REQUIRE(hit.has_value());
        CHECK(hit->part == TimelineLayout::Part::InEdge);
    }

    SECTION("the out edge") {
        const auto hit = layout.hitTest(f.sequence(), static_cast<std::int32_t>(endX) - 2, v1Y);
        REQUIRE(hit.has_value());
        CHECK(hit->part == TimelineLayout::Part::OutEdge);
    }

    SECTION("a gap is not a hit") {
        CHECK_FALSE(layout.hitTest(f.sequence(), 200, v1Y).has_value());
        CHECK_FALSE(layout.hitTest(f.sequence(), 800, v1Y).has_value());
    }

    SECTION("an empty track is not a hit") {
        CHECK_FALSE(layout.hitTest(f.sequence(), 350, 30).has_value());
    }

    SECTION("the headers and ruler are not hits") {
        CHECK_FALSE(layout.hitTest(f.sequence(), 40, v1Y).has_value());
        CHECK_FALSE(layout.hitTest(f.sequence(), 350, 5).has_value());
    }
}

TEST_CASE("Trim handles stay the same size however far you zoom", "[ui][timeline][hit]") {
    // Measured in pixels, not in time. A handle that shrinks as you zoom out is
    // one nobody can grab.
    testing::Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(25, 250))));
    TimelineLayout layout = makeLayout();

    for (const double zoom : {0.25, 1.0, 8.0}) {
        TimelineLayout copy = layout;
        copy.zoomBy(zoom, 150.0, kRate);
        const double startX = copy.xForTime(at(25));

        // Two pixels inside the edge is a handle at every zoom level.
        const auto edge = copy.hitTest(f.sequence(), static_cast<std::int32_t>(startX) + 2, 100);
        REQUIRE(edge.has_value());
        INFO("zoom " << zoom);
        CHECK(edge->part == TimelineLayout::Part::InEdge);
    }
}

TEST_CASE("A very short clip still has a reachable body", "[ui][timeline][hit]") {
    // With generous edge zones on both sides, a narrow clip could otherwise be
    // all handle and no body, and become impossible to select or drag.
    testing::Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(25, 2))));
    TimelineLayout layout = makeLayout();

    const double startX = layout.xForTime(at(25));
    const double endX = layout.xForTime(at(27));
    const auto middle = static_cast<std::int32_t>((startX + endX) * 0.5);

    const auto hit = layout.hitTest(f.sequence(), middle, 100);
    REQUIRE(hit.has_value());
    CHECK(hit->part == TimelineLayout::Part::Body);
}

TEST_CASE("The ruler step coarsens as you zoom out", "[ui][timeline]") {
    TimelineLayout layout = makeLayout();

    // Zoomed right in, the ruler counts frames.
    layout.zoomBy(40.0, 150.0, kRate);
    CHECK(layout.rulerStep(kRate).frames() == 1);

    // Zoomed out, it counts something a good deal coarser, and the steps only
    // ever grow as the zoom decreases.
    std::int64_t previous = layout.rulerStep(kRate).frames();
    for (int i = 0; i < 12; ++i) {
        layout.zoomBy(0.5, 150.0, kRate);
        const std::int64_t step = layout.rulerStep(kRate).frames();
        INFO("pixels per second " << layout.metrics().pixelsPerSecond << ", step " << step);
        CHECK(step >= previous);
        previous = step;
    }
    CHECK(previous > 25);  // coarser than a second
}

TEST_CASE("A rubber band selects what it touches", "[ui][timeline][hit]") {
    testing::Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 25))));    // 150..250
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(25, 25))));   // 250..350
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(100, 25))));  // 550..650
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 25))));
    TimelineLayout layout = makeLayout();

    const std::int32_t v1Y = 100;
    const std::int32_t a1Y = 160;

    SECTION("across two clips on one track") {
        const auto hits = layout.hitTestRect(f.sequence(), 200, v1Y - 10, 300, v1Y + 10);
        CHECK(hits.size() == 2);
    }

    SECTION("a clip only partly inside still counts") {
        // Requiring full containment would leave out the long clip the band was
        // obviously aimed at.
        const auto hits = layout.hitTestRect(f.sequence(), 260, v1Y - 5, 280, v1Y + 5);
        REQUIRE(hits.size() == 1);
        CHECK(hits[0].clip == f.track(f.v1).clips()[1].id);
    }

    SECTION("spanning tracks picks up both") {
        const auto hits = layout.hitTestRect(f.sequence(), 160, v1Y, 240, a1Y);
        CHECK(hits.size() == 2);
    }

    SECTION("dragged right to left is the same band") {
        const auto forward = layout.hitTestRect(f.sequence(), 200, v1Y - 10, 300, v1Y + 10);
        const auto backward = layout.hitTestRect(f.sequence(), 300, v1Y + 10, 200, v1Y - 10);
        CHECK(forward.size() == backward.size());
    }

    SECTION("a band over empty timeline selects nothing") {
        CHECK(layout.hitTestRect(f.sequence(), 700, v1Y - 10, 900, v1Y + 10).empty());
    }

    SECTION("a band entirely in the headers selects nothing") {
        CHECK(layout.hitTestRect(f.sequence(), 0, v1Y - 10, 100, v1Y + 10).empty());
    }
}

TEST_CASE("keyframes are hit in the lane along the bottom of a clip", "[hit][keyframes]") {
    testing::Fixture f;
    model::Clip placed = f.clip(100, 50, 500);
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), placed)));

    model::Clip* clip = f.track(f.v1).find(placed.id);
    REQUIRE(clip != nullptr);
    const auto keyAt = [&](std::int64_t timelineFrame, model::Param param, double value) {
        model::Keyframe key;
        key.time = clip->sourceTimeAt(f.at(timelineFrame));
        key.value = value;
        clip->animation.curve(param).set(key);
        return key.time;
    };
    const auto first = keyAt(110, model::Param::Opacity, 0.0);
    keyAt(110, model::Param::ScaleX, 2.0);  // two parameters, one instant
    const auto second = keyAt(130, model::Param::Opacity, 1.0);

    const TimelineLayout layout = makeLayout();

    // Two parameters keyed together are one diamond, not two.
    const auto times = TimelineLayout::keyframeTimes(*clip);
    REQUIRE(times.size() == 2);
    CHECK(times[0] == first);
    CHECK(times[1] == second);

    const auto rows = layout.rows(f.sequence());
    REQUIRE_FALSE(rows.empty());
    const auto row = rows.front();
    const auto x = static_cast<std::int32_t>(layout.xForTime(f.at(110)));
    const std::int32_t laneY = row.top + row.height - 2;

    const auto hit = layout.hitTestKeyframe(f.sequence(), x, laneY);
    REQUIRE(hit.has_value());
    CHECK(hit->clip == placed.id);
    CHECK(hit->time == first);

    // The middle of the clip is where its name and waveform live, so nothing is
    // grabbed there.
    CHECK_FALSE(layout.hitTestKeyframe(f.sequence(), x, row.top + (row.height / 2)).has_value());
    // And a point in the lane far from any keyframe is not a near miss.
    const auto empty = static_cast<std::int32_t>(layout.xForTime(f.at(122)));
    CHECK_FALSE(layout.hitTestKeyframe(f.sequence(), empty, laneY).has_value());
    // Aiming near one still catches it: a keyframe grabbable only on its exact
    // pixel is a keyframe nobody can drag.
    CHECK(layout.hitTestKeyframe(f.sequence(), x + 3, laneY).has_value());
}

TEST_CASE("a clip with no animation has nothing in its keyframe lane", "[hit][keyframes]") {
    testing::Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(100, 50, 500))));

    const TimelineLayout layout = makeLayout();
    const auto row = layout.rows(f.sequence()).front();
    const auto x = static_cast<std::int32_t>(layout.xForTime(f.at(110)));
    CHECK_FALSE(layout.hitTestKeyframe(f.sequence(), x, row.top + row.height - 2).has_value());
    // Nor do the headers or the ruler, which are not part of any clip.
    CHECK_FALSE(layout.hitTestKeyframe(f.sequence(), 10, row.top + row.height - 2).has_value());
}
