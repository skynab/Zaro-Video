#include <cmath>
#include <cstdint>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/render/AudioGraph.h"
#include "zaro/core/render/Downmix.h"

#include "ModelFixtures.h"
#include "TestSources.h"

using namespace zaro;
using Catch::Approx;
using zaro::testing::ConstantAudioSource;
using zaro::testing::Fixture;
using zaro::testing::RampAudioSource;

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

TEST_CASE("A keyframed gain ramps the mix", "[render][audio][animation]") {
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 1.0F, 2);
    render::AudioGraph graph{source};

    // Source time, and this clip's source starts at frame 500 of a 25fps
    // sequence: 20 seconds in.
    model::Clip clip = f.clip(0, 100, 500);
    const auto key = [](double seconds, double value) {
        model::Keyframe out;
        out.time =
            time::RationalTime{static_cast<std::int64_t>(seconds * 48000), time::rates::hz48000};
        out.value = value;
        return out;
    };
    clip.animation.curve(model::Param::GainDb).set(key(20.0, 0.0));
    clip.animation.curve(model::Param::GainDb).set(key(21.0, -20.0));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), clip)));

    auto mixed = graph.mix(f.sequence(), samples(0), 48000);
    REQUIRE(mixed);
    CHECK(mixed->channel(0)[0] == Approx(1.0F).margin(1e-4));
    CHECK(mixed->channel(0)[47999] == Approx(render::gainFromDb(-20.0)).margin(1e-3));
    // Monotonically down, sample by sample, with no plateau: a gain updated
    // once per block would hold still for hundreds of samples at a time.
    for (std::int64_t i = 1; i < 48000; ++i) {
        REQUIRE(mixed->channel(0)[i] < mixed->channel(0)[i - 1]);
    }
}

TEST_CASE("Automation does not depend on the audio device's block size",
          "[render][audio][animation]") {
    // The reason automation is evaluated per sample. A gain held constant
    // across a block steps at the block boundary, and the block boundary comes
    // from the audio device's buffer size rather than from the edit: the same
    // project would sound different on different hardware, which is not a thing
    // a mix is allowed to do.
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 1.0F, 2);
    render::AudioGraph graph{source};

    model::Clip clip = f.clip(0, 100, 500);
    const auto key = [](double seconds, double value, model::Interpolation how) {
        model::Keyframe out;
        out.time =
            time::RationalTime{static_cast<std::int64_t>(seconds * 48000), time::rates::hz48000};
        out.value = value;
        out.interpolation = how;
        return out;
    };
    clip.animation.curve(model::Param::GainDb).set(key(20.0, 0.0, model::Interpolation::Bezier));
    clip.animation.curve(model::Param::GainDb).set(key(20.25, -12.0, model::Interpolation::Linear));
    clip.animation.curve(model::Param::Pan).set(key(20.0, -1.0, model::Interpolation::Linear));
    clip.animation.curve(model::Param::Pan).set(key(20.25, 1.0, model::Interpolation::Linear));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), clip)));

    constexpr std::int64_t kTotal = 12000;
    auto whole = graph.mix(f.sequence(), samples(0), kTotal);
    REQUIRE(whole);

    for (std::int64_t block : {64, 128, 512, 1000}) {
        std::vector<float> pieced;
        pieced.reserve(static_cast<std::size_t>(kTotal));
        for (std::int64_t at = 0; at < kTotal; at += block) {
            const std::int64_t count = std::min(block, kTotal - at);
            auto part = graph.mix(f.sequence(), samples(at), count);
            REQUIRE(part);
            for (std::int64_t i = 0; i < count; ++i) {
                pieced.push_back(part->channel(0)[i]);
            }
        }
        REQUIRE(pieced.size() == static_cast<std::size_t>(kTotal));
        for (std::int64_t i = 0; i < kTotal; ++i) {
            // Bit-identical, not approximately equal. Any difference at all is
            // the block size leaking into the result.
            REQUIRE(pieced[static_cast<std::size_t>(i)] == whole->channel(0)[i]);
        }
    }
}

TEST_CASE("Solo silences everything that is not soloed", "[render][audio][mixer]") {
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 1.0F, 2);
    render::AudioGraph graph{source};

    const auto a2 = f.project.ids().next<model::TrackTag>();
    f.sequence().addTrack(a2, model::TrackKind::Audio, "A2");
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(a2), f.clip(0, 100, 500))));

    // Two tracks of the same signal: twice the level.
    auto both = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(both);
    CHECK(both->channel(0)[0] == Approx(2.0F).margin(1e-4));

    f.sequence().findTrack(f.a1)->setSoloed(true);
    auto onlyA1 = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(onlyA1);
    CHECK(onlyA1->channel(0)[0] == Approx(1.0F).margin(1e-4));

    // Soloing the second as well brings it back: solo is a set, not a switch.
    f.sequence().findTrack(a2)->setSoloed(true);
    auto bothSoloed = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(bothSoloed);
    CHECK(bothSoloed->channel(0)[0] == Approx(2.0F).margin(1e-4));
}

TEST_CASE("A dissolve on a sound track crossfades it", "[render][audio][mixer][transition]") {
    // The regression this guards: a dissolve dropped on a sound track was
    // drawn on the timeline, saved in the project, and did nothing at all to
    // what came out of the mixer -- the two clips butted, at full level,
    // exactly as if it were not there.
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 1.0F, 2);
    render::AudioGraph graph{source};

    // Two clips meeting at frame 50, both well inside their media so there are
    // handles either side of the cut to fade across.
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(50, 50, 600))));
    REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.a1), f.at(50), f.at(10))));

    // Frame 50 is the cut and the middle of a ten-frame dissolve centred on
    // it, so both halves are at cos(pi/4) == sin(pi/4) and sum to sqrt(2).
    // Equal power: two correlated takes hold their level across the join
    // instead of dipping in the middle the way a linear pair does.
    const std::int64_t middle = 50 * kSamplesPerFrame;
    auto atCut = graph.mix(f.sequence(), samples(middle), 1);
    REQUIRE(atCut);
    CHECK(atCut->channel(0)[0] == Approx(std::sqrt(2.0F)).margin(1e-3));

    // The far ends of the span are one clip alone, at full level.
    const std::int64_t before = 44 * kSamplesPerFrame;
    auto ahead = graph.mix(f.sequence(), samples(before), 1);
    REQUIRE(ahead);
    CHECK(ahead->channel(0)[0] == Approx(1.0F).margin(1e-3));

    // And a quarter of the way through -- frame 47.5 of a span running 45 to
    // 55 -- the halves are cos and sin of an eighth of a turn, not half each.
    const std::int64_t quarter = 47 * kSamplesPerFrame + (kSamplesPerFrame / 2);
    auto part = graph.mix(f.sequence(), samples(quarter), 1);
    REQUIRE(part);
    const double eighthTurn = std::acos(-1.0) * 0.125;
    CHECK(part->channel(0)[0] ==
          Approx(static_cast<float>(std::cos(eighthTurn) + std::sin(eighthTurn))).margin(1e-2));
}

TEST_CASE("A fade out on a sound clip goes to silence", "[render][audio][mixer][transition]") {
    // The dissolve button at the tail of a clip, which is what "fade out"
    // means and what there was previously no way to ask for: a clip with
    // nothing after it has no cut to dissolve across.
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 1.0F, 2);
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.a1), f.at(50), f.at(10))));
    REQUIRE(f.track(f.a1).transitions().front().isFadeOut());

    // Before the span, untouched.
    auto ahead = graph.mix(f.sequence(), samples(35 * kSamplesPerFrame), 1);
    REQUIRE(ahead);
    CHECK(ahead->channel(0)[0] == Approx(1.0F).margin(1e-4));

    // The span runs frames 40 to 50. Half way through it is cos of an eighth
    // turn; at the last sample it has all but gone.
    auto middle = graph.mix(f.sequence(), samples(45 * kSamplesPerFrame), 1);
    REQUIRE(middle);
    CHECK(middle->channel(0)[0] ==
          Approx(static_cast<float>(std::cos(std::acos(-1.0) * 0.25))).margin(1e-3));

    auto last = graph.mix(f.sequence(), samples((50 * kSamplesPerFrame) - 1), 1);
    REQUIRE(last);
    CHECK(last->channel(0)[0] < 0.01F);

    // And it descends the whole way, rather than stepping at a block boundary.
    float previous = 2.0F;
    for (std::int64_t frame = 40; frame < 50; ++frame) {
        auto block = graph.mix(f.sequence(), samples(frame * kSamplesPerFrame), 1);
        REQUIRE(block);
        CHECK(block->channel(0)[0] < previous);
        previous = block->channel(0)[0];
    }
}

TEST_CASE("A fade in on a sound clip comes up from silence", "[render][audio][mixer][transition]") {
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 1.0F, 2);
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.a1), f.at(0), f.at(10))));
    REQUIRE(f.track(f.a1).transitions().front().isFadeIn());

    auto first = graph.mix(f.sequence(), samples(0), 1);
    REQUIRE(first);
    CHECK(first->channel(0)[0] < 0.01F);

    auto after = graph.mix(f.sequence(), samples(20 * kSamplesPerFrame), 1);
    REQUIRE(after);
    CHECK(after->channel(0)[0] == Approx(1.0F).margin(1e-4));
}

TEST_CASE("A stretched dissolve fades over its new length", "[render][audio][mixer][transition]") {
    // The point of making the span draggable: the length of the span is the
    // length of the fade, so where the old span had finished the new one is
    // still going.
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 1.0F, 2);
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(50, 50, 600))));
    REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.a1), f.at(50), f.at(10))));
    const model::TransitionId id = f.track(f.a1).transitions().front().id;

    // Frame 42 is outside a ten-frame dissolve centred on the cut and inside a
    // thirty-frame one.
    const std::int64_t probe = 42 * kSamplesPerFrame;
    auto tight = graph.mix(f.sequence(), samples(probe), 1);
    REQUIRE(tight);
    CHECK(tight->channel(0)[0] == Approx(1.0F).margin(1e-3));

    REQUIRE(f.run(edit::makeSetTransitionRange(f.project, f.on(f.a1), id,
                                               time::TimeRange{f.at(35), f.at(30)})));
    auto wide = graph.mix(f.sequence(), samples(probe), 1);
    REQUIRE(wide);
    // Part way through now, so the outgoing half is already coming down and
    // the incoming half has started -- neither is at unity.
    CHECK(wide->channel(0)[0] != Approx(1.0F).margin(1e-3));
    CHECK(wide->channel(0)[0] > 1.0F);
}

TEST_CASE("Soloing a video track leaves the mix alone", "[render][audio][mixer]") {
    // The bug this guards shipped silent files. Solo was asked across both
    // lists at once, so isolating a shot on V1 -- an entirely ordinary thing
    // to do while cutting -- made every audio track fail the solo test, and an
    // export with that solo still set came out with a perfect picture and no
    // sound at all. Picture and sound are two independent solo groups.
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 1.0F, 2);
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 500))));

    auto before = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(before);
    CHECK(before->channel(0)[0] == Approx(1.0F).margin(1e-4));

    f.sequence().findTrack(f.v1)->setSoloed(true);
    auto after = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(after);
    CHECK(after->channel(0)[0] == Approx(1.0F).margin(1e-4));
}

TEST_CASE("Solo is scoped to the kind of track it is on", "[model][sequence]") {
    // The same rule from the other side, and the one the render graph reads to
    // decide what to draw: a soloed audio track must not blank the picture.
    Fixture f;
    f.sequence().findTrack(f.a1)->setSoloed(true);
    CHECK(f.sequence().hasSolo(model::TrackKind::Audio));
    CHECK_FALSE(f.sequence().hasSolo(model::TrackKind::Video));
    CHECK(f.sequence().isAudible(f.track(f.v1)));
    CHECK(f.sequence().isAudible(f.track(f.v2)));
    CHECK(f.sequence().isAudible(f.track(f.a1)));

    f.sequence().findTrack(f.a1)->setSoloed(false);
    f.sequence().findTrack(f.v1)->setSoloed(true);
    CHECK(f.sequence().hasSolo(model::TrackKind::Video));
    CHECK_FALSE(f.sequence().hasSolo(model::TrackKind::Audio));
    CHECK(f.sequence().isAudible(f.track(f.a1)));
    CHECK(f.sequence().isAudible(f.track(f.v1)));
    CHECK_FALSE(f.sequence().isAudible(f.track(f.v2)));
}

TEST_CASE("Mute wins over solo", "[render][audio][mixer]") {
    // Soloing a track someone has muted and hearing it anyway would make mute
    // mean nothing.
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 1.0F, 2);
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 500))));

    f.sequence().findTrack(f.a1)->setMuted(true);
    f.sequence().findTrack(f.a1)->setSoloed(true);
    auto mixed = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(mixed);
    CHECK(mixed->channel(0)[0] == Approx(0.0F).margin(1e-6));
}

TEST_CASE("Meters read the peak of each track and of the sum", "[render][audio][mixer]") {
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 0.5F, 2);
    render::AudioGraph graph{source};

    const auto a2 = f.project.ids().next<model::TrackTag>();
    f.sequence().addTrack(a2, model::TrackKind::Audio, "A2");
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(a2), f.clip(0, 100, 500))));
    // One track pulled down, so the two do not read the same.
    f.sequence().findTrack(a2)->setGainDb(-6.0);

    auto mixed = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(mixed);

    const render::AudioGraph::Meters& meters = graph.meters();
    CHECK(meters.peakFor(f.a1) == Approx(0.5F).margin(1e-4));
    CHECK(meters.peakFor(a2) == Approx(0.5F * render::gainFromDb(-6.0)).margin(1e-4));

    // The master accounts for the two adding up, which is the whole reason it
    // is measured on the sum rather than taken as the loudest track.
    CHECK(meters.masterPeak() > meters.peakFor(f.a1));
    CHECK(meters.masterPeak() == Approx(0.5F + (0.5F * render::gainFromDb(-6.0))).margin(1e-4));

    // A silent track reads zero rather than not appearing at all: a strip with
    // no meter looks broken, and "silent" is a reading.
    f.sequence().findTrack(a2)->setMuted(true);
    REQUIRE(graph.mix(f.sequence(), samples(0), 480));
    CHECK(graph.meters().peakFor(a2) == Approx(0.0F));
}

TEST_CASE("Meters follow the picture, not the whole clip", "[render][audio][mixer]") {
    // Measured per block, so a meter reflects the moment the playhead is at
    // rather than the loudest thing anywhere in the timeline.
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 1.0F, 2);
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 25, 500))));

    REQUIRE(graph.mix(f.sequence(), samples(0), 480));
    CHECK(graph.meters().peakFor(f.a1) == Approx(1.0F).margin(1e-4));

    // Past the end of the clip there is nothing to hear.
    REQUIRE(graph.mix(f.sequence(), samples(kSamplesPerFrame * 30), 480));
    CHECK(graph.meters().peakFor(f.a1) == Approx(0.0F));
}

TEST_CASE("A retimed clip's audio is retimed with it", "[render][audio][speed]") {
    // Without this the picture retimes and the sound does not, which is drift
    // that grows for as long as the clip lasts -- the exact failure the whole
    // rational-time discipline exists to avoid.
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 1.0F, 2);
    render::AudioGraph graph{source};

    model::Clip clip = f.clip(0, 25, 500);
    clip.sourceRange = f.range(500, 50);  // twice as fast
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), clip)));
    REQUIRE(f.track(f.a1).clips().front().speed() == Approx(2.0));

    // The clip is half as long on the timeline, so it fills half of what it
    // used to and there is silence after it.
    auto during = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(during);
    CHECK(during->channel(0)[0] == Approx(1.0F).margin(1e-4));

    auto after = graph.mix(f.sequence(), samples(kSamplesPerFrame * 30), 480);
    REQUIRE(after);
    CHECK(after->channel(0)[0] == Approx(0.0F).margin(1e-6));

    // And it is still there right up to its new out point.
    auto justInside = graph.mix(f.sequence(), samples(kSamplesPerFrame * 24), 480);
    REQUIRE(justInside);
    CHECK(justInside->channel(0)[0] == Approx(1.0F).margin(1e-4));
}

TEST_CASE("A reversed clip still produces sound", "[render][audio][speed]") {
    // The read runs backwards through the source, which is easy to get wrong in
    // a way that reads past the start and returns silence.
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 0.8F, 2);
    render::AudioGraph graph{source};

    model::Clip clip = f.clip(0, 25, 500);
    clip.reversed = true;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), clip)));

    for (const std::int64_t frame : {0, 5, 12, 24}) {
        auto mixed = graph.mix(f.sequence(), samples(kSamplesPerFrame * frame), 480);
        REQUIRE(mixed);
        INFO("frame " << frame);
        CHECK(mixed->channel(0)[0] == Approx(0.8F).margin(1e-3));
    }
}

TEST_CASE("A sequence is measured through the mix it will deliver", "[render][audio][loudness]") {
    // Faders, pans, automation, the processing chain and every clip gain are
    // all in it. Measuring the clips instead would give a number about the
    // material rather than about the programme.
    Fixture f;
    ConstantAudioSource source;
    // A constant is a rectangle rather than a tone, but its level is exactly
    // known, which is what makes the arithmetic below checkable.
    source.define(f.longMedia, 0.1F, 2);
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 250, 500))));

    const time::TimeRange span{f.at(0), f.at(200)};
    auto measured = graph.measureLoudness(f.sequence(), span);
    REQUIRE(measured);
    const double plain = measured->integratedLufs;
    CHECK(plain > render::LoudnessMeter::kSilence);
    CHECK(measured->samplePeakDbfs == Approx(-20.0).margin(0.5));

    // Pulling the fader down six decibels moves the measurement by six.
    f.sequence().findTrack(f.a1)->setGainDb(-6.0);
    auto quieter = graph.measureLoudness(f.sequence(), span);
    REQUIRE(quieter);
    CHECK(quieter->integratedLufs == Approx(plain - 6.0).margin(0.2));

    // And the gain that would reach a target is the difference.
    CHECK(quieter->gainToReach(-23.0) == Approx(-23.0 - quieter->integratedLufs).margin(1e-6));
}

TEST_CASE("Silence has no gain that would make it loud", "[render][audio][loudness]") {
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 0.0F, 2);
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 250, 500))));

    auto measured = graph.measureLoudness(f.sequence(), time::TimeRange{f.at(0), f.at(200)});
    REQUIRE(measured);
    CHECK(measured->integratedLufs == Approx(render::LoudnessMeter::kSilence));
    // Not an enormous number: there is no amount of gain that makes silence
    // reach a target, and offering one would be a button that ruins a mix.
    CHECK(measured->gainToReach(-23.0) == Approx(0.0));
}

TEST_CASE("An empty range is refused rather than measured as silence",
          "[render][audio][loudness]") {
    // Those are different answers: one is "this programme is quiet" and the
    // other is "you asked about nothing".
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 0.5F, 2);
    render::AudioGraph graph{source};
    CHECK_FALSE(graph.measureLoudness(f.sequence(), time::TimeRange{f.at(10), f.at(0)}));
}

TEST_CASE("A clip's own compressor pulls it down before the track sees it", "[render][audio]") {
    // Per-clip processing is the repair a track's channel strip cannot do: one
    // take louder than the rest of the scene, on the same track as the rest of
    // the scene. What this checks is that it is applied at all, and that it is
    // applied to the clip rather than to everything on the track.
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 1.0F, 2);
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 500))));

    const auto clipId = f.sequence().findTrack(f.a1)->clips().front().id;
    auto plain = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(plain);
    const float before = plain->channel(0)[479];
    CHECK(before == Approx(1.0F).margin(1e-5));

    model::Compressor squash;
    squash.enabled = true;
    // Well under the signal, hard, and fast enough to have taken hold by the
    // end of a 10ms block.
    squash.thresholdDb = -24.0;
    squash.ratio = 20.0;
    squash.attackMs = 1.0;
    squash.releaseMs = 50.0;
    squash.makeupDb = 0.0;
    REQUIRE(f.run(
        edit::makeSetClipProcessing(f.project, f.on(f.a1), clipId, model::AudioEq{}, squash)));

    graph.resetProcessing();
    auto squashed = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(squashed);
    const float after = squashed->channel(0)[479];
    CHECK(after < before * 0.5F);

    // And undoing it puts the level back, which is what says the graph is
    // reading the clip rather than holding its own copy.
    f.stack.undo(f.project);
    graph.resetProcessing();
    auto restored = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(restored);
    CHECK(restored->channel(0)[479] == Approx(before).margin(1e-5));
}

TEST_CASE("A clip's high pass takes the bottom off that clip alone", "[render][audio]") {
    // Two clips on one track, one filtered. A steady signal through a high pass
    // decays towards zero -- that is what "no energy below the corner" means
    // for a constant -- so the filtered clip drops away and its neighbour does
    // not move at all.
    Fixture f;
    ConstantAudioSource source;
    source.define(f.longMedia, 1.0F, 2);
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 100))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(200, 100, 100))));

    const auto firstClip = f.sequence().findTrack(f.a1)->clips().front().id;

    model::AudioEq eq;
    eq.enabled = true;
    eq.highPassHz = 2000.0;
    REQUIRE(f.run(
        edit::makeSetClipProcessing(f.project, f.on(f.a1), firstClip, eq, model::Compressor{})));

    graph.resetProcessing();
    auto filtered = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(filtered);
    CHECK(std::fabs(filtered->channel(0)[479]) < 0.2F);

    // The second clip starts at frame 200.
    graph.resetProcessing();
    auto neighbour = graph.mix(f.sequence(), samples(200 * kSamplesPerFrame), 480);
    REQUIRE(neighbour);
    CHECK(neighbour->channel(0)[479] == Approx(1.0F).margin(1e-5));
}

TEST_CASE("A 5.1 clip keeps its dialogue in a stereo sequence", "[render][audio]") {
    // The bug this guards was silent and severe: the mixer took the first N
    // channels, so a 5.1 file kept L and R and dropped C, LFE, Ls and Rs. In a
    // 5.1 mix the centre channel is the dialogue -- the picture and the music
    // arrived and the words did not, with nothing in the interface to say so.
    Fixture f;
    ConstantAudioSource source;
    // L R C LFE Ls Rs. Only the centre carries anything, which is the case the
    // old code got exactly wrong.
    source.defineChannels(f.longMedia, {0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F});
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 500))));

    auto mixed = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(mixed);
    // Both sides, at -3 dB, which is what ATSC A/52 specifies for the fold.
    CHECK(mixed->channel(0)[0] == Approx(render::DownmixMatrix::kFold).margin(1e-4));
    CHECK(mixed->channel(1)[0] == Approx(render::DownmixMatrix::kFold).margin(1e-4));
}

TEST_CASE("A 5.1 clip's LFE does not reach the stereo bus", "[render][audio]") {
    // Band-limited rumble for a driver the stereo bus does not have. Folding it
    // in adds energy nobody mixed and nothing can reproduce.
    Fixture f;
    ConstantAudioSource source;
    source.defineChannels(f.longMedia, {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F});
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 500))));

    auto mixed = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(mixed);
    CHECK(mixed->channel(0)[0] == Approx(0.0F).margin(1e-6));
    CHECK(mixed->channel(1)[0] == Approx(0.0F).margin(1e-6));
}

TEST_CASE("A 5.1 clip's surrounds stay on their own side", "[render][audio]") {
    Fixture f;
    ConstantAudioSource source;
    // Left surround only.
    source.defineChannels(f.longMedia, {0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F});
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 500))));

    auto mixed = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(mixed);
    CHECK(mixed->channel(0)[0] == Approx(render::DownmixMatrix::kFold).margin(1e-4));
    CHECK(mixed->channel(1)[0] == Approx(0.0F).margin(1e-6));
}

TEST_CASE("A stereo clip is untouched by the fold", "[render][audio]") {
    // The common path, and the one that must not move: a matched layout passes
    // through at unity, exactly as it did before any of this existed.
    Fixture f;
    ConstantAudioSource source;
    source.defineChannels(f.longMedia, {1.0F, 0.25F});
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 500))));

    auto mixed = graph.mix(f.sequence(), samples(0), 480);
    REQUIRE(mixed);
    CHECK(mixed->channel(0)[0] == Approx(1.0F).margin(1e-5));
    CHECK(mixed->channel(1)[0] == Approx(0.25F).margin(1e-5));
}

TEST_CASE("Mixing is sample accurate whatever the block size", "[render][audio]") {
    // The bug this guards made playback sound robotic, and nothing else caught
    // it: every offline path mixes in whole video frames, where the fault is
    // invisible. The audio device does not. It asks for 1024 samples at a time,
    // and 1024 is not a whole frame at any normal rate -- so a mapping that
    // answers in source *frames* hands back the same 1920 samples for two
    // blocks running and then jumps one frame. Held up to a ramp, that is a
    // flat spot followed by a step; through a speaker it is a buzz at the block
    // rate.
    //
    // So: read the same range twice, once frame aligned and once in device
    // sized blocks, and require both to be the ramp itself.
    Fixture f;
    RampAudioSource source;
    source.define(f.longMedia, 2);
    render::AudioGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 500))));

    // The clip starts 500 frames into its media, so timeline sample 0 is source
    // sample 500 * 1920.
    const std::int64_t sourceOrigin = 500 * kSamplesPerFrame;
    constexpr std::int64_t kTotal = 12000;  // not a whole number of frames

    const auto gather = [&](std::int64_t blockSize) {
        std::vector<float> out;
        out.reserve(static_cast<std::size_t>(kTotal));
        for (std::int64_t at = 0; at < kTotal; at += blockSize) {
            const std::int64_t count = std::min(blockSize, kTotal - at);
            auto mixed = graph.mix(f.sequence(), samples(at), count);
            REQUIRE(mixed);
            REQUIRE(mixed->sampleCount() == count);
            for (std::int64_t i = 0; i < count; ++i) {
                out.push_back(mixed->channel(0)[i]);
            }
        }
        return out;
    };

    const std::vector<float> frameAligned = gather(kSamplesPerFrame);
    const std::vector<float> deviceSized = gather(1024);
    REQUIRE(frameAligned.size() == static_cast<std::size_t>(kTotal));
    REQUIRE(deviceSized.size() == static_cast<std::size_t>(kTotal));

    for (std::int64_t i = 0; i < kTotal; ++i) {
        const auto expected = static_cast<float>(sourceOrigin + i);
        const auto index = static_cast<std::size_t>(i);
        if (frameAligned[index] != Approx(expected)) {
            FAIL("frame aligned mix is wrong at sample " << i << ": got " << frameAligned[index]
                                                         << ", expected " << expected);
        }
        if (deviceSized[index] != Approx(expected)) {
            FAIL("1024 sample blocks gave sample "
                 << i << " as " << deviceSized[index] << ", expected " << expected
                 << " -- the source mapping is rounding to whole frames");
        }
    }
}
