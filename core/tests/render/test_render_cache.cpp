#include <cstdint>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/render/RenderCache.h"
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

/// A fixture whose sequence is small enough to composite thousands of times.
struct CacheFixture : Fixture {
    SolidFrameSource source{16, 16};
    render::RenderCache cache;
    render::RenderGraph graph{source};

    CacheFixture() {
        sequence().setSize(16, 16);
        source.define(longMedia, opaque(1.0F, 0.0F, 0.0F));
        source.define(shortMedia, opaque(0.0F, 0.0F, 1.0F));
        graph.setProject(&project);
        graph.setRenderCache(&cache);
    }

    [[nodiscard]] std::uint64_t recipe(std::int64_t frame) {
        return render::frameRecipe(&project, sequence(), at(frame));
    }
    model::Clip& clipOn(model::TrackId id, std::size_t index = 0) {
        return const_cast<model::Clip&>(track(id).clips()[index]);
    }
};

}  // namespace

TEST_CASE("A frame composited twice is composited once", "[render][cache]") {
    CacheFixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 100))));

    auto first = f.graph.composite(f.sequence(), f.at(10));
    REQUIRE(first);
    CHECK(f.cache.count() == 1);
    CHECK(f.cache.misses() == 1);

    auto second = f.graph.composite(f.sequence(), f.at(10));
    REQUIRE(second);
    CHECK(f.cache.hits() == 1);
    // Identical, not merely similar: a cache that returns something close is
    // worse than no cache, because the difference only shows up in the export.
    CHECK(second->at(8, 8).r == Approx(first->at(8, 8).r));
    CHECK(second->at(8, 8).a == Approx(first->at(8, 8).a));
    // And the diagnostics come back with it.
    CHECK(f.graph.lastClipCount() == 1);
}

TEST_CASE("Changing what is on a frame invalidates it", "[render][cache]") {
    CacheFixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 100))));

    auto before = f.graph.composite(f.sequence(), f.at(10));
    REQUIRE(before);
    CHECK(before->at(8, 8).r == Approx(1.0F));

    // Half opacity on the clip that shows at frame 10.
    f.clipOn(f.v1).transform.opacity = 0.5;

    auto after = f.graph.composite(f.sequence(), f.at(10));
    REQUIRE(after);
    CHECK(f.cache.stale() == 1);
    CHECK(after->at(8, 8).a == Approx(0.5F));
}

TEST_CASE("Changing something elsewhere on the timeline keeps the frame", "[render][cache]") {
    CacheFixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(200, 50))));

    REQUIRE(f.graph.composite(f.sequence(), f.at(10)));
    const std::uint64_t was = f.recipe(10);

    // The far clip is graded into the ground. Frame 10 cannot see it.
    f.clipOn(f.v1, 1).transform.opacity = 0.0;

    CHECK(f.recipe(10) == was);
    REQUIRE(f.graph.composite(f.sequence(), f.at(10)));
    CHECK(f.cache.hits() == 1);
    CHECK(f.cache.stale() == 0);
}

TEST_CASE("The recipe covers everything the frame is made of", "[render][cache]") {
    // The one test that guards against drift: whenever the picture at an
    // instant changes, the recipe for that instant must change too. Each
    // section changes one thing and requires both to move.
    CacheFixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 100))));

    auto pixelAt = [&f]() {
        render::RenderGraph plain{f.source};
        plain.setProject(&f.project);
        auto frame = plain.composite(f.sequence(), f.at(10));
        REQUIRE(frame);
        return frame->at(8, 8);
    };

    const std::uint64_t wasRecipe = f.recipe(10);
    const render::Rgba wasPixel = pixelAt();

    SECTION("a grade on the clip") {
        f.clipOn(f.v1).color.exposure = -1.0;
    }
    SECTION("the clip's opacity") {
        f.clipOn(f.v1).transform.opacity = 0.25;
    }
    SECTION("the clip being disabled") {
        f.clipOn(f.v1).enabled = false;
    }
    SECTION("the track being hidden") {
        f.sequence().findTrack(f.v1)->setMuted(true);
    }
    SECTION("the clip's source range") {
        f.clipOn(f.v1).source = f.shortMedia;
        f.clipOn(f.v1).sourceRange = f.range(0, 100);
    }
    SECTION("the file the media points at") {
        f.project.mediaMutable()[0].path = "/media/somewhere-else.mov";
        f.source.define(f.longMedia, opaque(0.0F, 1.0F, 0.0F));
    }

    CHECK(f.recipe(10) != wasRecipe);
    const render::Rgba now = pixelAt();
    CHECK(
        (now.r != wasPixel.r || now.g != wasPixel.g || now.b != wasPixel.b || now.a != wasPixel.a));
}

TEST_CASE("Turning proxies on invalidates every frame", "[render][cache]") {
    CacheFixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 100))));
    const std::uint64_t was = f.recipe(10);

    // Not part of any clip, and it decides which file every one of them reads.
    f.project.setUsingProxies(true);
    CHECK(f.recipe(10) != was);
}

TEST_CASE("A nested sequence's contents are part of the outer recipe", "[render][cache]") {
    CacheFixture f;
    model::Sequence inner{f.project.ids().next<model::SequenceTag>(), "inner", time::rates::fps25};
    inner.setSize(16, 16);
    const model::TrackId innerTrack = f.project.ids().next<model::TrackTag>();
    inner.addTrack(innerTrack, model::TrackKind::Video, "V1");
    const model::SequenceId innerId = f.project.addSequence(std::move(inner));
    REQUIRE(f.run(edit::makeOverwrite(f.project, {innerId, innerTrack}, f.clip(0, 100))));

    // Source start 0, so the outer frame reads the inner sequence at the same
    // instant it is asked for.
    model::Clip host = f.clip(0, 100, 0);
    host.nested = innerId;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), host)));

    const std::uint64_t was = f.recipe(10);
    const_cast<model::Clip&>(f.project.findSequence(innerId)->findTrack(innerTrack)->clips()[0])
        .transform.opacity = 0.5;
    CHECK(f.recipe(10) != was);
}

TEST_CASE("Burned-in captions are part of the recipe", "[render][cache]") {
    CacheFixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 100))));
    const std::uint64_t withoutCaptions = f.recipe(10);

    f.sequence().captions().setBurnedIn(true);
    CHECK(f.recipe(10) != withoutCaptions);
    const std::uint64_t burnedIn = f.recipe(10);

    model::Caption caption;
    caption.range = time::TimeRange{time::RationalTime{0, time::Rational{1000, 1}},
                                    time::RationalTime{4000, time::Rational{1000, 1}}};
    caption.text = "hello";
    f.sequence().captions().add(caption);
    CHECK(f.recipe(10) != burnedIn);
}

TEST_CASE("The cache is bounded by bytes, not by frames", "[render][cache]") {
    render::RenderCache cache{4 * 16 * 16 * 4 * static_cast<std::size_t>(sizeof(float))};
    CacheFixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 100))));
    f.graph.setRenderCache(&cache);

    for (std::int64_t i = 0; i < 20; ++i) {
        REQUIRE(f.graph.composite(f.sequence(), f.at(i)));
    }
    CHECK(cache.byteSize() <= cache.budgetBytes());
    CHECK(cache.count() < 20);
    // Strict LRU: the frames just rendered are the ones kept.
    CHECK(cache.contains(f.sequenceId, f.at(19), f.recipe(19)));
    CHECK_FALSE(cache.contains(f.sequenceId, f.at(0), f.recipe(0)));
}

TEST_CASE("Pre-rendering fills a range, and the second pass has nothing to do", "[render][cache]") {
    CacheFixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 100))));

    auto first = render::prerender(f.graph, f.cache, &f.project, f.sequence(), f.range(0, 30));
    REQUIRE(first);
    CHECK(first->rendered == 30);
    CHECK(first->alreadyCached == 0);

    auto second = render::prerender(f.graph, f.cache, &f.project, f.sequence(), f.range(0, 30));
    REQUIRE(second);
    CHECK(second->rendered == 0);
    CHECK(second->alreadyCached == 30);

    SECTION("and playing it back is all hits") {
        f.cache.resetStatistics();
        for (std::int64_t i = 0; i < 30; ++i) {
            REQUIRE(f.graph.composite(f.sequence(), f.at(i)));
        }
        CHECK(f.cache.hits() == 30);
        CHECK(f.cache.misses() == 0);
    }
}

TEST_CASE("A cancelled pre-render keeps what it managed", "[render][cache]") {
    CacheFixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 100))));

    auto stats = render::prerender(f.graph, f.cache, &f.project, f.sequence(), f.range(0, 30),
                                   [](std::int32_t done, std::int32_t) { return done < 10; });
    REQUIRE(stats);
    CHECK(stats->cancelled);
    CHECK(stats->rendered == 10);
    // Stopping is not undoing: the ten frames are cached and stay cached.
    CHECK(f.cache.count() == 10);
    CHECK(f.cache.contains(f.sequenceId, f.at(9), f.recipe(9)));
}

TEST_CASE("Cached spans describe what a bar would draw", "[render][cache]") {
    CacheFixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 200))));
    REQUIRE(render::prerender(f.graph, f.cache, &f.project, f.sequence(), f.range(40, 40)));

    const auto spans = render::cachedSpans(f.cache, &f.project, f.sequence(), f.range(0, 200), 200);
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].start().frames() == 40);
    CHECK(spans[0].duration().frames() == 40);

    SECTION("and an edit under the span takes it away") {
        f.clipOn(f.v1).transform.opacity = 0.5;
        const auto after =
            render::cachedSpans(f.cache, &f.project, f.sequence(), f.range(0, 200), 200);
        CHECK(after.empty());
    }
}

TEST_CASE("Evicting a sequence leaves the others alone", "[render][cache]") {
    CacheFixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 100))));
    REQUIRE(render::prerender(f.graph, f.cache, &f.project, f.sequence(), f.range(0, 10)));
    CHECK(f.cache.count() == 10);

    f.cache.evict(model::SequenceId{999});
    CHECK(f.cache.count() == 10);
    f.cache.evict(f.sequenceId);
    CHECK(f.cache.count() == 0);
    CHECK(f.cache.byteSize() == 0);
}
