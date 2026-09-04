#include <cstdint>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/render/RenderGraph.h"

#include "ModelFixtures.h"
#include "TestSources.h"

using namespace zaro;
using Catch::Approx;
using zaro::testing::Fixture;
using zaro::testing::SolidFrameSource;

namespace {

render::Rgba opaque(float r, float g, float b) {
    return render::Rgba{r, g, b, 1.0F};
}

}  // namespace

TEST_CASE("Compositing an empty sequence gives a transparent frame", "[render][graph]") {
    Fixture f;
    f.sequence().setSize(16, 16);
    SolidFrameSource source{16, 16};
    render::RenderGraph graph{source};

    auto frame = graph.composite(f.sequence(), f.at(0));
    REQUIRE(frame);
    CHECK(frame->width() == 16);
    // Transparent, not black: an empty sequence is empty, and the difference
    // matters the moment anything is exported with alpha.
    CHECK(frame->at(8, 8).a == 0.0F);
    CHECK(graph.lastClipCount() == 0);
}

TEST_CASE("A clip renders where it sits on the timeline", "[render][graph]") {
    Fixture f;
    f.sequence().setSize(16, 16);
    SolidFrameSource source{16, 16};
    source.define(f.longMedia, opaque(1.0F, 0.0F, 0.0F));
    render::RenderGraph graph{source};

    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 25, 500))));

    SECTION("inside the clip") {
        auto frame = graph.composite(f.sequence(), f.at(60));
        REQUIRE(frame);
        CHECK(frame->at(8, 8).r == Approx(1.0F));
        CHECK(graph.lastClipCount() == 1);
    }

    SECTION("in the gap before it") {
        auto frame = graph.composite(f.sequence(), f.at(10));
        REQUIRE(frame);
        CHECK(frame->at(8, 8).a == 0.0F);
    }

    SECTION("exactly on its last frame") {
        auto frame = graph.composite(f.sequence(), f.at(74));
        REQUIRE(frame);
        CHECK(frame->at(8, 8).a == Approx(1.0F));
    }

    SECTION("one past its end") {
        auto frame = graph.composite(f.sequence(), f.at(75));
        REQUIRE(frame);
        CHECK(frame->at(8, 8).a == 0.0F);
    }
}

TEST_CASE("The right source frame is requested", "[render][graph]") {
    // Timeline time has to map to source time through the clip, and getting
    // that wrong is invisible in the pixels of a flat test pattern -- so assert
    // on the request itself.
    Fixture f;
    f.sequence().setSize(8, 8);
    SolidFrameSource source{8, 8};
    source.define(f.longMedia, opaque(1.0F, 1.0F, 1.0F));
    render::RenderGraph graph{source};

    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(100, 50, 500))));

    REQUIRE(graph.composite(f.sequence(), f.at(100)));
    REQUIRE(graph.composite(f.sequence(), f.at(110)));
    REQUIRE(graph.composite(f.sequence(), f.at(149)));

    REQUIRE(source.requests().size() == 3);
    CHECK(source.requests()[0].frames() == 500);
    CHECK(source.requests()[1].frames() == 510);
    CHECK(source.requests()[2].frames() == 549);
}

TEST_CASE("Video tracks composite bottom-up", "[render][graph]") {
    Fixture f;
    f.sequence().setSize(8, 8);
    SolidFrameSource source{8, 8};
    source.define(f.longMedia, opaque(1.0F, 0.0F, 0.0F));   // V1: red
    source.define(f.shortMedia, opaque(0.0F, 0.0F, 1.0F));  // V2: blue
    render::RenderGraph graph{source};

    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 0, f.longMedia))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v2), f.clip(0, 50, 0, f.shortMedia))));

    auto frame = graph.composite(f.sequence(), f.at(10));
    REQUIRE(frame);
    // V2 is above V1 and opaque, so blue wins.
    CHECK(frame->at(4, 4).b == Approx(1.0F));
    CHECK(frame->at(4, 4).r == Approx(0.0F));
    CHECK(graph.lastClipCount() == 2);

    SECTION("and a half-transparent upper track lets the lower one through") {
        const model::ClipId upper = f.track(f.v2).clips()[0].id;
        auto* clip = f.track(f.v2).find(upper);
        REQUIRE(clip != nullptr);
        clip->transform.opacity = 0.5;

        auto blended = graph.composite(f.sequence(), f.at(10));
        REQUIRE(blended);
        CHECK(blended->at(4, 4).b == Approx(0.5F));
        CHECK(blended->at(4, 4).r == Approx(0.5F));
    }
}

TEST_CASE("Muted tracks and disabled clips do not render", "[render][graph]") {
    Fixture f;
    f.sequence().setSize(8, 8);
    SolidFrameSource source{8, 8};
    source.define(f.longMedia, opaque(1.0F, 1.0F, 1.0F));
    render::RenderGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));

    SECTION("muted track") {
        f.track(f.v1).setMuted(true);
        auto frame = graph.composite(f.sequence(), f.at(10));
        REQUIRE(frame);
        CHECK(frame->at(4, 4).a == 0.0F);
    }

    SECTION("disabled clip") {
        auto* clip = f.track(f.v1).find(f.track(f.v1).clips()[0].id);
        clip->enabled = false;
        auto frame = graph.composite(f.sequence(), f.at(10));
        REQUIRE(frame);
        CHECK(frame->at(4, 4).a == 0.0F);
    }
}

TEST_CASE("Audio tracks are not composited into the picture", "[render][graph]") {
    Fixture f;
    f.sequence().setSize(8, 8);
    SolidFrameSource source{8, 8};
    source.define(f.longMedia, opaque(1.0F, 1.0F, 1.0F));
    render::RenderGraph graph{source};

    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 50, 500))));
    auto frame = graph.composite(f.sequence(), f.at(10));
    REQUIRE(frame);
    CHECK(frame->at(4, 4).a == 0.0F);
    CHECK(graph.lastClipCount() == 0);
}

TEST_CASE("An unreadable clip leaves a hole, not a failed render", "[render][graph]") {
    // A missing frame should be visible and diagnosable. Failing the whole
    // render turns one bad clip into a stalled edit.
    Fixture f;
    f.sequence().setSize(8, 8);
    SolidFrameSource source{8, 8};  // nothing defined
    render::RenderGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));

    auto frame = graph.composite(f.sequence(), f.at(10));
    REQUIRE(frame);
    CHECK(frame->at(4, 4).a == 0.0F);
    CHECK(graph.lastClipCount() == 0);
}

TEST_CASE("Compositing is deterministic and reuses its buffer", "[render][graph]") {
    Fixture f;
    f.sequence().setSize(8, 8);
    SolidFrameSource source{8, 8};
    source.define(f.longMedia, opaque(0.25F, 0.5F, 0.75F));
    render::RenderGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));

    render::RgbaImage reused;
    REQUIRE(graph.compositeInto(f.sequence(), f.at(10), reused).ok());
    const render::Rgba first = reused.at(4, 4);

    // Same time, same answer, and rendering a gap into the same buffer must
    // clear what was there rather than leaving the previous frame behind.
    REQUIRE(graph.compositeInto(f.sequence(), f.at(10), reused).ok());
    CHECK(reused.at(4, 4) == first);

    REQUIRE(graph.compositeInto(f.sequence(), f.at(500), reused).ok());
    CHECK(reused.at(4, 4).a == 0.0F);
}

TEST_CASE("A cross dissolve blends the two clips it joins", "[render][graph][transition]") {
    Fixture f;
    f.sequence().setSize(8, 8);
    SolidFrameSource source{8, 8};
    source.define(f.longMedia, opaque(1.0F, 0.0F, 0.0F));   // outgoing: red
    source.define(f.shortMedia, opaque(0.0F, 0.0F, 1.0F));  // incoming: blue
    render::RenderGraph graph{source};

    // Both clips start inside their media, so there are handles either side of
    // the cut for the dissolve to reach into.
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500, f.longMedia))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 20, f.shortMedia))));
    REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(50), f.at(10))));
    // Range is 45 to 55.

    SECTION("before it, only the outgoing clip") {
        auto frame = graph.composite(f.sequence(), f.at(40));
        REQUIRE(frame);
        CHECK(frame->at(4, 4).r == Approx(1.0F));
        CHECK(frame->at(4, 4).b == Approx(0.0F));
    }

    SECTION("at the start, essentially all outgoing") {
        auto frame = graph.composite(f.sequence(), f.at(45));
        REQUIRE(frame);
        CHECK(frame->at(4, 4).r == Approx(1.0F));
        CHECK(frame->at(4, 4).b == Approx(0.0F).margin(0.01));
        CHECK(graph.lastClipCount() == 2);  // both contributed
    }

    SECTION("half way, half of each") {
        auto frame = graph.composite(f.sequence(), f.at(50));
        REQUIRE(frame);
        CHECK(frame->at(4, 4).r == Approx(0.5F).margin(0.01));
        CHECK(frame->at(4, 4).b == Approx(0.5F).margin(0.01));
    }

    SECTION("near the end, essentially all incoming") {
        auto frame = graph.composite(f.sequence(), f.at(54));
        REQUIRE(frame);
        CHECK(frame->at(4, 4).b == Approx(0.9F).margin(0.02));
        CHECK(frame->at(4, 4).r == Approx(0.1F).margin(0.02));
    }

    SECTION("after it, only the incoming clip") {
        auto frame = graph.composite(f.sequence(), f.at(60));
        REQUIRE(frame);
        CHECK(frame->at(4, 4).b == Approx(1.0F));
        CHECK(frame->at(4, 4).r == Approx(0.0F));
    }

    SECTION("the outgoing clip is read past its out point, into its handles") {
        // At frame 52 the outgoing clip has already ended on the timeline, but
        // the dissolve still shows it -- from source frames beyond where the
        // clip stops. If it were clamped at the cut the picture would freeze
        // for the second half of every dissolve.
        REQUIRE(graph.composite(f.sequence(), f.at(52)));
        bool sawPastTheCut = false;
        for (const time::RationalTime& request : source.requests()) {
            if (request.frames() > 550) {  // clip ends at source frame 550
                sawPastTheCut = true;
            }
        }
        CHECK(sawPastTheCut);
    }
}

TEST_CASE("A keyframed opacity fades the composited frame", "[render][graph][animation]") {
    Fixture f;
    f.sequence().setSize(16, 16);
    SolidFrameSource source{16, 16};
    source.define(f.longMedia, opaque(1.0F, 1.0F, 1.0F));
    render::RenderGraph graph{source};

    // Keyframes are in source time, and this clip's source starts at frame 500,
    // so a fade over the clip's first second lives at 500..525.
    model::Clip clip = f.clip(100, 50, 500);
    const auto key = [](std::int64_t sourceFrame, double value) {
        model::Keyframe out;
        out.time = time::RationalTime{sourceFrame, time::rates::fps25};
        out.value = value;
        return out;
    };
    clip.animation.curve(model::Param::Opacity).set(key(500, 0.0));
    clip.animation.curve(model::Param::Opacity).set(key(525, 1.0));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), clip)));

    // Premultiplied, so the stored value is the opacity itself against a
    // transparent background.
    CHECK(graph.composite(f.sequence(), f.at(100))->at(8, 8).a == Approx(0.0F).margin(1e-5));
    CHECK(graph.composite(f.sequence(), f.at(112))->at(8, 8).a == Approx(0.48F).margin(0.02));
    CHECK(graph.composite(f.sequence(), f.at(125))->at(8, 8).a == Approx(1.0F).margin(1e-5));
    // Held past the last keyframe rather than extrapolated to 2.0.
    CHECK(graph.composite(f.sequence(), f.at(140))->at(8, 8).a == Approx(1.0F).margin(1e-5));
}

TEST_CASE("Animation follows the picture when a clip is moved and trimmed",
          "[render][graph][animation]") {
    // The reason keyframes are stored in source time. A fade set on a frame
    // has to stay on that frame when the clip is rippled down the timeline or
    // its head is trimmed off.
    Fixture f;
    f.sequence().setSize(16, 16);
    SolidFrameSource source{16, 16};
    source.define(f.longMedia, opaque(1.0F, 1.0F, 1.0F));
    render::RenderGraph graph{source};

    model::Clip clip = f.clip(100, 50, 500);
    const auto key = [](std::int64_t sourceFrame, double value) {
        model::Keyframe out;
        out.time = time::RationalTime{sourceFrame, time::rates::fps25};
        out.value = value;
        return out;
    };
    clip.animation.curve(model::Param::Opacity).set(key(500, 0.0));
    clip.animation.curve(model::Param::Opacity).set(key(520, 1.0));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), clip)));

    const float halfway = graph.composite(f.sequence(), f.at(110))->at(8, 8).a;
    CHECK(halfway == Approx(0.5F).margin(0.02));

    // Source frame 510 is now at timeline frame 210.
    REQUIRE(f.run(edit::makeMove(f.project, f.on(f.v1), clip.id, f.v1, f.at(200))));
    CHECK(graph.composite(f.sequence(), f.at(210))->at(8, 8).a == Approx(halfway).margin(1e-5));

    // Trim ten frames off the head. Source frame 510 is now the clip's first
    // frame, and the fade is half done there.
    REQUIRE(f.run(edit::makeTrim(f.project, f.on(f.v1), clip.id, edit::Edge::In, f.at(10))));
    CHECK(graph.composite(f.sequence(), f.at(210))->at(8, 8).a == Approx(halfway).margin(1e-5));
}

TEST_CASE("Both halves of a transition are graded", "[render][graph][grade]") {
    // The outgoing half went two phases without its colour correction: each of
    // the three draw sites carried its own copy of the grade setup, and a patch
    // meant to update all three matched only two. They share one path now, and
    // this is the test that says so.
    Fixture f;
    f.sequence().setSize(16, 16);
    SolidFrameSource source{16, 16};
    source.define(f.longMedia, opaque(1.0F, 1.0F, 1.0F));
    render::RenderGraph graph{source};

    model::Clip first = f.clip(0, 50, 500);
    model::Clip second = f.clip(50, 50, 600);
    // Both graded down hard, so a half that is not graded shows as a bright
    // frame rather than as a subtly wrong one.
    first.color.exposure = -3.0;
    second.color.exposure = -3.0;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), first)));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), second)));
    REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(50), f.at(10))));

    // Inside the dissolve both clips contribute. Ungraded white is 1.0; three
    // stops down is an eighth of that.
    const auto during = graph.composite(f.sequence(), f.at(48));
    REQUIRE(during);
    CHECK(graph.lastClipCount() == 2);
    CHECK(during->at(8, 8).r < 0.2F);

    // And each half on its own, away from the transition.
    CHECK(graph.composite(f.sequence(), f.at(10))->at(8, 8).r < 0.2F);
    CHECK(graph.composite(f.sequence(), f.at(80))->at(8, 8).r < 0.2F);
}

TEST_CASE("A secondary corrects only what its qualifier selects", "[render][graph][secondary]") {
    Fixture f;
    f.sequence().setSize(16, 16);
    SolidFrameSource source{16, 16};
    // A saturated red frame.
    source.define(f.longMedia, opaque(0.8F, 0.05F, 0.05F));
    render::RenderGraph graph{source};

    model::Clip clip = f.clip(0, 50, 500);
    clip.secondary.qualifier.enabled = true;
    clip.secondary.qualifier.hueCentre = 120.0;  // greens, which this frame has none of
    clip.secondary.qualifier.hueWidth = 30.0;
    clip.secondary.qualifier.hueSoftness = 5.0;
    clip.secondary.correction.exposure = -4.0;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), clip)));

    // Nothing selected, so nothing changes.
    const auto untouched = graph.composite(f.sequence(), f.at(10));
    REQUIRE(untouched);
    CHECK(untouched->at(8, 8).r == Approx(0.8F).epsilon(0.01));

    // Aim the same window at red and it takes effect.
    model::Clip* placed = f.track(f.v1).find(clip.id);
    placed->secondary.qualifier.hueCentre = 0.0;
    const auto keyed = graph.composite(f.sequence(), f.at(10));
    REQUIRE(keyed);
    CHECK(keyed->at(8, 8).r < 0.1F);

    // And the mask view shows the selection rather than the picture.
    placed->secondary.showMask = true;
    const auto mask = graph.composite(f.sequence(), f.at(10));
    REQUIRE(mask);
    CHECK(mask->at(8, 8).r == Approx(1.0F).margin(0.01));
    CHECK(mask->at(8, 8).g == Approx(mask->at(8, 8).r));
    CHECK(mask->at(8, 8).b == Approx(mask->at(8, 8).r));
}

TEST_CASE("A graphic clip composites without any media", "[render][graph][shape]") {
    Fixture f;
    f.sequence().setSize(64, 64);
    // A source that would fail if asked: a graphic must never reach it.
    SolidFrameSource source{64, 64};
    render::RenderGraph graph{source};

    model::Graphic shape;
    shape.kind = model::GraphicKind::Rectangle;
    shape.width = 20;
    shape.height = 20;
    shape.red = 1.0;
    shape.green = 0.0;
    shape.blue = 0.0;
    REQUIRE(f.run(edit::makeAddGraphic(f.project, f.on(f.v1), shape, f.range(0, 50))));

    const auto frame = graph.composite(f.sequence(), f.at(10));
    REQUIRE(frame);
    CHECK(graph.lastClipCount() == 1);
    CHECK(frame->at(32, 32).r == Approx(1.0F));
    CHECK(frame->at(32, 32).a == Approx(1.0F));
    // And nothing outside it, since the rest of a graphic clip is transparent.
    CHECK(frame->at(2, 2).a == Approx(0.0F));
}

TEST_CASE("A graphic obeys the transform, the grade and the fade", "[render][graph][shape]") {
    // The reason a graphic is a clip rather than a separate kind of thing: all
    // of this already works, and none of it had to be taught about shapes.
    Fixture f;
    f.sequence().setSize(64, 64);
    SolidFrameSource source{64, 64};
    render::RenderGraph graph{source};

    model::Graphic shape;
    shape.kind = model::GraphicKind::Rectangle;
    shape.width = 20;
    shape.height = 20;
    REQUIRE(f.run(edit::makeAddGraphic(f.project, f.on(f.v1), shape, f.range(0, 50))));
    const model::ClipId id = f.track(f.v1).clips().front().id;

    // Half opacity.
    model::Transform faded;
    faded.opacity = 0.5;
    REQUIRE(f.run(edit::makeSetTransform(f.project, f.on(f.v1), id, faded)));
    CHECK(graph.composite(f.sequence(), f.at(10))->at(32, 32).a == Approx(0.5F));

    // Two stops down, on top of that.
    model::ColorCorrection darker;
    darker.exposure = -2.0;
    REQUIRE(f.run(edit::makeSetColorCorrection(f.project, f.on(f.v1), id, darker)));
    const auto graded = graph.composite(f.sequence(), f.at(10));
    REQUIRE(graded);
    // Alpha is coverage and exposure does not touch it; the colour is a quarter.
    CHECK(graded->at(32, 32).a == Approx(0.5F));
    CHECK(graded->at(32, 32).r / graded->at(32, 32).a == Approx(0.25F).margin(1e-4));

    // And moving it moves the shape.
    model::Transform moved = faded;
    moved.positionX = 16.0;
    REQUIRE(f.run(edit::makeSetTransform(f.project, f.on(f.v1), id, moved)));
    const auto shifted = graph.composite(f.sequence(), f.at(10));
    REQUIRE(shifted);
    CHECK(shifted->at(48, 32).a > 0.4F);
    CHECK(shifted->at(32, 32).a == Approx(0.0F).margin(0.01));
}

TEST_CASE("Speed is the ratio between a clip's two ranges", "[render][graph][speed]") {
    // Not a stored number. Every trim and every retime already maintains the
    // ranges, and a `speed` field would be a second source of truth that the
    // first disagreement would leave playing at one rate and laid out at
    // another.
    Fixture f;
    model::Clip clip = f.clip(0, 25, 500);
    clip.sourceRange = f.range(500, 50);  // fifty frames of source in twenty-five
    CHECK(clip.speed() == Approx(2.0));

    clip.sourceRange = f.range(500, 25);
    CHECK(clip.speed() == Approx(1.0));

    clip.timelineRange = f.range(0, 50);
    CHECK(clip.speed() == Approx(0.5));
}

TEST_CASE("A retimed clip reads its source faster", "[render][graph][speed]") {
    Fixture f;
    model::Clip clip = f.clip(0, 25, 500);
    clip.sourceRange = f.range(500, 50);

    // Halfway along the clip is halfway through the source it covers.
    CHECK(clip.sourceTimeAt(f.at(0)).frames() == 500);
    CHECK(clip.sourceTimeAt(f.at(12)).frames() == Approx(524).margin(1));
    CHECK(clip.sourceTimeAt(f.at(24)).frames() == Approx(548).margin(1));
}

TEST_CASE("A reversed clip plays its last frame first", "[render][graph][speed]") {
    Fixture f;
    model::Clip clip = f.clip(0, 25, 500);
    clip.reversed = true;

    // The out point is exclusive, so the first frame shown is the one before
    // it -- reading the out point itself would be reading the frame after the
    // clip.
    CHECK(clip.sourceTimeAt(f.at(0)).frames() == 524);
    CHECK(clip.sourceTimeAt(f.at(24)).frames() == Approx(500).margin(1));

    // And the two mappings stay inverses of each other, which is what keeps a
    // keyframe on the frame it was set on.
    for (std::int64_t frame = 0; frame < 25; ++frame) {
        const auto source = clip.sourceTimeAt(f.at(frame));
        CHECK(std::llabs(clip.timelineTimeOf(source).frames() - frame) <= 1);
    }
}

TEST_CASE("Retiming a clip ripples what follows", "[render][graph][speed]") {
    // Without it, speeding a clip up leaves a hole and slowing it down runs
    // over its neighbour.
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 900))));
    const model::ClipId first = f.track(f.v1).clips().front().id;

    REQUIRE(f.run(edit::makeSetSpeed(f.project, f.on(f.v1), first, 2.0, false)));
    const model::Track& track = f.track(f.v1);
    REQUIRE(track.clips().size() == 2);
    CHECK(track.clips()[0].duration().frames() == 25);
    CHECK(track.clips()[0].speed() == Approx(2.0));
    // The cut stays closed.
    CHECK(track.clips()[1].start().frames() == 25);

    // The source range is untouched: a retime changes how long the clip
    // occupies the timeline, not which frames it covers.
    CHECK(track.clips()[0].sourceRange.duration().frames() == 50);
}

TEST_CASE("Speed has to be a positive number", "[render][graph][speed]") {
    // Direction is the reversed flag: a speed of -2 and a reversed speed of 2
    // would be two ways to say one thing, and the pair would disagree.
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    const model::ClipId id = f.track(f.v1).clips().front().id;

    CHECK_FALSE(edit::makeSetSpeed(f.project, f.on(f.v1), id, -2.0, false));
    CHECK_FALSE(edit::makeSetSpeed(f.project, f.on(f.v1), id, 0.0, false));
    // Fast enough to leave nothing behind is refused rather than silently
    // producing a clip of no length.
    CHECK_FALSE(edit::makeSetSpeed(f.project, f.on(f.v1), id, 10000.0, false));
}

TEST_CASE("A reversed clip composites its frames backwards", "[render][graph][speed]") {
    Fixture f;
    f.sequence().setSize(16, 16);
    SolidFrameSource source{16, 16};
    // A source whose colour says which frame it is.
    source.defineRamp(f.longMedia);
    render::RenderGraph graph{source};

    model::Clip clip = f.clip(0, 25, 500);
    clip.reversed = true;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), clip)));

    const float first = graph.composite(f.sequence(), f.at(0))->at(8, 8).r;
    const float last = graph.composite(f.sequence(), f.at(24))->at(8, 8).r;
    CHECK(first > last);
}

TEST_CASE("A nested sequence composites as a clip", "[render][graph][nest]") {
    Fixture f;
    f.sequence().setSize(32, 32);
    SolidFrameSource source{32, 32};
    source.define(f.longMedia, opaque(1.0F, 0.0F, 0.0F));
    render::RenderGraph graph{source};
    graph.setProject(&f.project);

    // An inner sequence with a red clip on it.
    model::Sequence inner{f.project.ids().next<model::SequenceTag>(), "inner", time::rates::fps25};
    inner.setSize(32, 32);
    const model::SequenceId innerId = inner.id();
    const auto innerTrack = f.project.ids().next<model::TrackTag>();
    inner.addTrack(innerTrack, model::TrackKind::Video, "V1");
    f.project.addSequence(std::move(inner));
    REQUIRE(f.run(edit::makeOverwrite(f.project, {innerId, innerTrack}, f.clip(0, 50, 500))));

    // And an outer clip that is the whole of it.
    model::Clip nest = f.clip(0, 50, 0);
    nest.nested = innerId;
    nest.sourceRange = f.range(0, 50);
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), nest)));

    const auto frame = graph.composite(f.sequence(), f.at(10));
    REQUIRE(frame);
    CHECK(frame->at(16, 16).r == Approx(1.0F));
    CHECK(frame->at(16, 16).a == Approx(1.0F));
    // One clip on this sequence, whatever the nested one contains.
    CHECK(graph.lastClipCount() == 1);
}

TEST_CASE("A nested clip obeys the transform and the grade", "[render][graph][nest]") {
    // The reason a nest is a clip rather than a special case: everything that
    // works on a clip works on it, and none of that code was told about nesting.
    Fixture f;
    f.sequence().setSize(32, 32);
    SolidFrameSource source{32, 32};
    source.define(f.longMedia, opaque(1.0F, 1.0F, 1.0F));
    render::RenderGraph graph{source};
    graph.setProject(&f.project);

    model::Sequence inner{f.project.ids().next<model::SequenceTag>(), "inner", time::rates::fps25};
    inner.setSize(32, 32);
    const model::SequenceId innerId = inner.id();
    const auto innerTrack = f.project.ids().next<model::TrackTag>();
    inner.addTrack(innerTrack, model::TrackKind::Video, "V1");
    f.project.addSequence(std::move(inner));
    REQUIRE(f.run(edit::makeOverwrite(f.project, {innerId, innerTrack}, f.clip(0, 50, 500))));

    model::Clip nest = f.clip(0, 50, 0);
    nest.nested = innerId;
    nest.sourceRange = f.range(0, 50);
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), nest)));
    const model::ClipId id = f.track(f.v1).clips().front().id;

    model::ColorCorrection darker;
    darker.exposure = -2.0;
    REQUIRE(f.run(edit::makeSetColorCorrection(f.project, f.on(f.v1), id, darker)));

    const auto frame = graph.composite(f.sequence(), f.at(10));
    REQUIRE(frame);
    CHECK(frame->at(16, 16).r == Approx(0.25F).margin(0.01));
}

TEST_CASE("A nested clip with no project resolves to nothing", "[render][graph][nest]") {
    // A graph that was never given a project renders everything else rather
    // than failing: that is what the headless tests do, and a missing sequence
    // is a hole rather than a stalled render.
    Fixture f;
    f.sequence().setSize(16, 16);
    SolidFrameSource source{16, 16};
    render::RenderGraph graph{source};

    model::Clip nest = f.clip(0, 50, 0);
    nest.nested = model::SequenceId{999};
    nest.sourceRange = f.range(0, 50);
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), nest)));

    const auto frame = graph.composite(f.sequence(), f.at(10));
    REQUIRE(frame);
    CHECK(frame->at(8, 8).a == Approx(0.0F));
    CHECK(graph.lastClipCount() == 0);
}

TEST_CASE("An adjustment layer grades what is beneath it", "[render][graph][adjustment]") {
    Fixture f;
    f.sequence().setSize(16, 16);
    SolidFrameSource source{16, 16};
    source.define(f.longMedia, opaque(1.0F, 1.0F, 1.0F));
    render::RenderGraph graph{source};

    // A white clip on V1, and an adjustment above it on V2.
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    CHECK(graph.composite(f.sequence(), f.at(10))->at(8, 8).r == Approx(1.0F));

    REQUIRE(f.run(edit::makeAddAdjustment(f.project, f.on(f.v2), f.range(0, 50))));
    const model::ClipId id = f.track(f.v2).clips().front().id;
    model::ColorCorrection darker;
    darker.exposure = -2.0;
    REQUIRE(f.run(edit::makeSetColorCorrection(f.project, f.on(f.v2), id, darker)));

    const auto frame = graph.composite(f.sequence(), f.at(10));
    REQUIRE(frame);
    CHECK(frame->at(8, 8).r == Approx(0.25F).margin(0.01));
    // It did not draw anything of its own: the alpha is still the clip's.
    CHECK(frame->at(8, 8).a == Approx(1.0F));
}

TEST_CASE("An adjustment layer leaves what it does not cover", "[render][graph][adjustment]") {
    Fixture f;
    f.sequence().setSize(16, 16);
    SolidFrameSource source{16, 16};
    source.define(f.longMedia, opaque(1.0F, 1.0F, 1.0F));
    render::RenderGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 100, 500))));
    REQUIRE(f.run(edit::makeAddAdjustment(f.project, f.on(f.v2), f.range(0, 20))));
    const model::ClipId id = f.track(f.v2).clips().front().id;

    model::ColorCorrection darker;
    darker.exposure = -3.0;
    REQUIRE(f.run(edit::makeSetColorCorrection(f.project, f.on(f.v2), id, darker)));

    // Inside its range it corrects; past its out point the picture is itself.
    CHECK(graph.composite(f.sequence(), f.at(10))->at(8, 8).r < 0.2F);
    CHECK(graph.composite(f.sequence(), f.at(50))->at(8, 8).r == Approx(1.0F));
}

TEST_CASE("An adjustment layer's opacity and mask limit it", "[render][graph][adjustment]") {
    // Blended rather than switched, so a partly opaque adjustment is a partial
    // correction -- which is how the control reads.
    Fixture f;
    f.sequence().setSize(32, 32);
    SolidFrameSource source{32, 32};
    source.define(f.longMedia, opaque(1.0F, 1.0F, 1.0F));
    render::RenderGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeAddAdjustment(f.project, f.on(f.v2), f.range(0, 50))));
    const model::ClipId id = f.track(f.v2).clips().front().id;

    model::ColorCorrection black;
    black.exposure = -8.0;
    REQUIRE(f.run(edit::makeSetColorCorrection(f.project, f.on(f.v2), id, black)));

    model::Transform half;
    half.opacity = 0.5;
    REQUIRE(f.run(edit::makeSetTransform(f.project, f.on(f.v2), id, half)));
    // Half of the way from white to nearly nothing.
    CHECK(graph.composite(f.sequence(), f.at(10))->at(16, 16).r == Approx(0.5F).margin(0.02));

    // A mask limits it to a region, and outside that the picture is untouched.
    model::Mask mask;
    mask.shape = model::MaskShape::Rectangle;
    mask.width = 8;
    mask.height = 8;
    REQUIRE(f.run(edit::makeSetMask(f.project, f.on(f.v2), id, mask)));
    const auto frame = graph.composite(f.sequence(), f.at(10));
    REQUIRE(frame);
    CHECK(frame->at(16, 16).r == Approx(0.5F).margin(0.02));
    CHECK(frame->at(2, 2).r == Approx(1.0F));
}

TEST_CASE("An adjustment layer over nothing does nothing", "[render][graph][adjustment]") {
    // There is no picture underneath to correct, and inventing one would mean a
    // black rectangle appearing wherever somebody put a layer early.
    Fixture f;
    f.sequence().setSize(16, 16);
    SolidFrameSource source{16, 16};
    render::RenderGraph graph{source};
    REQUIRE(f.run(edit::makeAddAdjustment(f.project, f.on(f.v2), f.range(0, 50))));
    const model::ClipId id = f.track(f.v2).clips().front().id;
    model::ColorCorrection darker;
    darker.exposure = -4.0;
    REQUIRE(f.run(edit::makeSetColorCorrection(f.project, f.on(f.v2), id, darker)));

    const auto frame = graph.composite(f.sequence(), f.at(10));
    REQUIRE(frame);
    CHECK(frame->at(8, 8).a == Approx(0.0F));
    CHECK(frame->at(8, 8).r == Approx(0.0F));
}

// A still animates when its properties are keyframed.
//
// This is the point of importing pictures at all: a photograph that can only
// sit there is a photograph, and one that can be pushed in on is a shot. It is
// also the part that was not obviously going to work. Keyframes are stored in
// *source* time (ADR 0008) so that a fade set on a frame stays on that frame
// through any trim -- and a still has one frame of source. Had a still clip
// been given a one-frame source range, every keyframe on it would map to the
// same instant and nothing would ever move.
//
// What makes it work is that a still's source range mirrors its timeline range,
// so source time advances across the clip exactly as it does for footage. This
// renders the same photograph at three points and checks the picture moves.
TEST_CASE("A keyframed still animates", "[render][graph][still]") {
    Fixture f;
    f.sequence().setSize(16, 16);
    const model::MediaRefId photo = f.addStill();
    SolidFrameSource source{16, 16};
    source.define(photo, opaque(1.0F, 0.0F, 0.0F));
    render::RenderGraph graph{source};

    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.stillClip(0, 100, photo))));
    const model::ClipId id = f.track(f.v1).clips().front().id;

    // Fade the picture up across the clip: opaque at the end, invisible at the
    // start. Keyframes are placed in source time, which for this clip runs from
    // zero to its own length.
    REQUIRE(f.run(
        edit::makeSetKeyframe(f.project, f.on(f.v1), id, model::Param::Opacity, f.at(0), 0.0)));
    REQUIRE(f.run(
        edit::makeSetKeyframe(f.project, f.on(f.v1), id, model::Param::Opacity, f.at(99), 1.0)));

    const auto alphaAt = [&](std::int64_t frame) {
        auto composited = graph.composite(f.sequence(), f.at(frame));
        REQUIRE(composited);
        return composited->at(8, 8).a;
    };

    const float start = alphaAt(0);
    const float middle = alphaAt(50);
    const float end = alphaAt(99);

    CHECK(start == Approx(0.0F).margin(1e-3));
    CHECK(end == Approx(1.0F).margin(1e-3));
    // The one that would fail if source time did not advance: a still whose
    // keyframes all landed on the same instant would hold one value throughout.
    CHECK(middle > start);
    CHECK(middle < end);
    CHECK(middle == Approx(0.5F).margin(0.05F));
}

TEST_CASE("A still shows the same picture at every frame", "[render][graph][still]") {
    // The other half of the bargain: source time advances so that animation
    // works, and the picture does *not* change with it, because there is only
    // one. A still asked for at frame 90 must not come back empty because the
    // source has run out.
    Fixture f;
    f.sequence().setSize(16, 16);
    const model::MediaRefId photo = f.addStill();
    SolidFrameSource source{16, 16};
    source.define(photo, opaque(0.0F, 1.0F, 0.0F));
    render::RenderGraph graph{source};

    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.stillClip(0, 100, photo))));

    for (const std::int64_t frame : {std::int64_t{0}, std::int64_t{50}, std::int64_t{99}}) {
        auto composited = graph.composite(f.sequence(), f.at(frame));
        REQUIRE(composited);
        CHECK(composited->at(8, 8).g == Approx(1.0F).margin(1e-3));
        CHECK(composited->at(8, 8).a == Approx(1.0F).margin(1e-3));
    }
}
