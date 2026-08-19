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
