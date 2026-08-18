#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/render/AudioGraph.h"

#include "ModelFixtures.h"
#include "TestSources.h"

using namespace zaro;
using Catch::Approx;
using zaro::testing::ConstantAudioSource;
using zaro::testing::Fixture;

namespace {

/// 48kHz against a 25fps sequence: 1920 samples per frame, exactly.
constexpr std::int64_t kSamplesPerFrame = 1920;

time::RationalTime samples(std::int64_t count) {
    return time::RationalTime{count, time::rates::hz48000};
}

}  // namespace

TEST_CASE("Decibels convert to linear gain", "[render][audio]") {
    CHECK(render::gainFromDb(0.0) == Approx(1.0F));
    CHECK(render::gainFromDb(-6.0) == Approx(0.5011872F).margin(1e-5));
    CHECK(render::gainFromDb(6.0) == Approx(1.9952624F).margin(1e-5));
    // Silence, not a denormal that costs a hundred times as much to multiply.
    CHECK(render::gainFromDb(-120.0) == 0.0F);
}

TEST_CASE("A centred stereo clip passes through at unity", "[render][audio]") {
    // Applying the pan law twice -- once at the clip, once at the track -- is
    // the bug this guards: it would make everything centred half as loud.
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 1.0F, 2);
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 500))));

    auto mixed = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(mixed);
    CHECK(mixed->channel(0)[0] == Approx(1.0F).margin(1e-5));
    CHECK(mixed->channel(1)[0] == Approx(1.0F).margin(1e-5));
}

TEST_CASE("A centred mono clip is spread at constant power", "[render][audio]") {
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 1.0F, 1);
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 500))));

    auto mixed = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(mixed);
    // Placed into the field, so 0.707 per side and unity total power.
    CHECK(mixed->channel(0)[0] == Approx(0.70710678F).margin(1e-5));
    CHECK(mixed->channel(1)[0] == Approx(0.70710678F).margin(1e-5));
}

TEST_CASE("Balance is unity at centre", "[render][audio]") {
    float left = 0.0F;
    float right = 0.0F;
    render::balanceGains(0.0, left, right);
    CHECK(left == Approx(1.0F));
    CHECK(right == Approx(1.0F));

    render::balanceGains(1.0, left, right);
    CHECK(left == Approx(0.0F));
    CHECK(right == Approx(1.0F));

    render::balanceGains(-0.5, left, right);
    CHECK(left == Approx(1.0F));
    CHECK(right == Approx(0.5F));
}

TEST_CASE("Pan is constant power", "[render][audio]") {
    float left = 0.0F;
    float right = 0.0F;

    SECTION("centred loses 3dB per side but keeps total power") {
        render::panGains(0.0, left, right);
        CHECK(left == Approx(0.70710678F).margin(1e-5));
        CHECK(right == Approx(0.70710678F).margin(1e-5));
        CHECK(left * left + right * right == Approx(1.0F).margin(1e-5));
    }

    SECTION("hard left and hard right") {
        render::panGains(-1.0, left, right);
        CHECK(left == Approx(1.0F).margin(1e-5));
        CHECK(right == Approx(0.0F).margin(1e-5));
        render::panGains(1.0, left, right);
        CHECK(right == Approx(1.0F).margin(1e-5));
    }

    SECTION("power stays flat across the whole sweep") {
        for (int step = -10; step <= 10; ++step) {
            render::panGains(step / 10.0, left, right);
            INFO("pan " << step / 10.0);
            CHECK(left * left + right * right == Approx(1.0F).margin(1e-5));
        }
    }
}

TEST_CASE("An empty sequence mixes to silence", "[render][audio]") {
    Fixture f;
    ConstantAudioSource source;
    render::AudioGraph graph{source};

    auto mixed = graph.mix(f.sequence(), f.at(0), 480);
    REQUIRE(mixed);
    CHECK(mixed->sampleCount() == 480);
    CHECK(mixed->channelCount() == 2);
    CHECK(mixed->peak(0) == 0.0F);
}

TEST_CASE("A clip contributes over exactly its own span", "[render][audio]") {
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 0.5F);
    render::AudioGraph graph{source};

    // Frames 10-20 at 25fps is samples 19200-38400 at 48kHz.
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(10, 10, 500))));

    auto mixed = graph.mix(f.sequence(), samples(0), 48000);
    REQUIRE(mixed);
    const float* left = mixed->channel(0);

    // The test source is stereo and centred, so it passes through at unity.
    const float expected = 0.5F;
    CHECK(left[0] == Approx(0.0F));
    CHECK(left[10 * kSamplesPerFrame - 1] == Approx(0.0F));
    CHECK(left[10 * kSamplesPerFrame] == Approx(expected).margin(1e-5));
    CHECK(left[20 * kSamplesPerFrame - 1] == Approx(expected).margin(1e-5));
    // Sample-accurate at the far edge too, not just the near one.
    CHECK(left[20 * kSamplesPerFrame] == Approx(0.0F));
}

TEST_CASE("Blocks tile without a gap or a repeated sample", "[render][audio]") {
    // The property that keeps audio from drifting against picture: mixing a
    // span in one call and in fifty must produce identical samples.
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 0.25F);
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 500))));

    constexpr std::int64_t kTotal = 48000;
    constexpr std::int64_t kBlock = 960;

    auto whole = graph.mix(f.sequence(), samples(0), kTotal);
    REQUIRE(whole);

    std::vector<float> pieced;
    pieced.reserve(static_cast<std::size_t>(kTotal));
    for (std::int64_t offset = 0; offset < kTotal; offset += kBlock) {
        auto block = graph.mix(f.sequence(), samples(offset), kBlock);
        REQUIRE(block);
        const float* channel = block->channel(0);
        pieced.insert(pieced.end(), channel, channel + kBlock);
    }

    REQUIRE(pieced.size() == static_cast<std::size_t>(kTotal));
    for (std::int64_t i = 0; i < kTotal; ++i) {
        if (pieced[static_cast<std::size_t>(i)] != whole->channel(0)[i]) {
            FAIL("sample " << i << " differs between one call and fifty");
        }
    }
    SUCCEED("48000 samples identical however they were blocked");
}

TEST_CASE("Clip and track gain multiply", "[render][audio]") {
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 1.0F);
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 500))));

    auto* clip = f.track(f.a1).find(f.track(f.a1).clips()[0].id);
    REQUIRE(clip != nullptr);
    clip->gainDb = -6.0;
    clip->pan = -1.0;  // hard left, so the centre-pan factor drops out
    f.track(f.a1).setGainDb(-6.0);
    f.track(f.a1).setPan(-1.0);

    auto mixed = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(mixed);
    // -6dB twice is -12dB overall.
    CHECK(mixed->channel(0)[0] == Approx(render::gainFromDb(-12.0)).margin(1e-5));
    CHECK(mixed->channel(1)[0] == Approx(0.0F).margin(1e-5));
}

TEST_CASE("Muted tracks and disabled clips are silent", "[render][audio]") {
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 1.0F);
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 500))));

    SECTION("muted track") {
        f.track(f.a1).setMuted(true);
        auto mixed = graph.mix(f.sequence(), samples(0), 480);
        REQUIRE(mixed);
        CHECK(mixed->peak(0) == 0.0F);
    }

    SECTION("disabled clip") {
        f.track(f.a1).find(f.track(f.a1).clips()[0].id)->enabled = false;
        auto mixed = graph.mix(f.sequence(), samples(0), 480);
        REQUIRE(mixed);
        CHECK(mixed->peak(0) == 0.0F);
    }
}

TEST_CASE("Overlapping clips on different tracks sum", "[render][audio]") {
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 0.25F);
    render::AudioGraph graph{source};

    const model::TrackId a2 = f.project.ids().next<model::TrackTag>();
    f.sequence().addTrack(a2, model::TrackKind::Audio, "A2");

    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, {f.sequenceId, a2}, f.clip(0, 100, 500))));

    auto mixed = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(mixed);
    CHECK(mixed->channel(0)[0] == Approx(2.0F * 0.25F).margin(1e-5));
    CHECK(graph.lastClipCount() == 2);
}

TEST_CASE("A short read fills the rest with silence", "[render][audio]") {
    // A clip trimmed to the very last sample of its media must not take the
    // whole mix down with it.
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 1.0F);
    source.setAvailableSamples(100);
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 500))));

    auto mixed = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(mixed);
    CHECK(mixed->channel(0)[0] > 0.0F);
    CHECK(mixed->channel(0)[99] > 0.0F);
    CHECK(mixed->channel(0)[100] == Approx(0.0F));
}

TEST_CASE("Unreadable audio is silence, not a failed mix", "[render][audio]") {
    Fixture f;
    ConstantAudioSource source;  // nothing defined
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 500))));

    auto mixed = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(mixed);
    CHECK(mixed->peak(0) == 0.0F);
}

TEST_CASE("The right source time is requested", "[render][audio]") {
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 1.0F);
    render::AudioGraph graph{source};
    // Clip at timeline frame 10, reading from source frame 500.
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(10, 50, 500))));

    // Ask for a block starting one frame into the clip.
    REQUIRE(graph.mix(f.sequence(), samples(11 * kSamplesPerFrame), 480));
    // Source frame 501 at 25fps, expressed in 48kHz samples.
    CHECK(source.lastSourceStart.rescaledTo(time::rates::hz48000).frames() ==
          501 * kSamplesPerFrame);
}
