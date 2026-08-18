#include <algorithm>
#include <cstdlib>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/playback/PlaybackScheduler.h"

using namespace zaro;
using playback::PlaybackScheduler;
using playback::PresentAction;

namespace {

PlaybackScheduler::Config config25(std::size_t capacity = 6, std::int64_t durationFrames = 1000) {
    PlaybackScheduler::Config config;
    config.frameRate = time::rates::fps25;
    config.audioRate = time::rates::hz48000;
    config.queueCapacity = capacity;
    config.duration = time::RationalTime{durationFrames, time::rates::fps25};
    return config;
}

time::RationalTime at(std::int64_t frame, const time::Rational& rate = time::rates::fps25) {
    return time::RationalTime{frame, rate};
}

/// A 2x2 image: the scheduler only cares about timestamps, so tests should not
/// pay for pixels.
render::RgbaImage tiny() {
    return render::RgbaImage{2, 2};
}

/// Fill the queue as far as it will go.
void renderAhead(PlaybackScheduler& scheduler) {
    while (const auto target = scheduler.nextRenderTarget()) {
        scheduler.submit(*target, tiny());
    }
}

}  // namespace

TEST_CASE("Position follows the audio clock exactly", "[playback][scheduler]") {
    PlaybackScheduler scheduler{config25()};
    scheduler.start(at(100), time::Rational::fromInt(1), 0);

    CHECK(scheduler.positionAt(0) == at(100));
    // 48000 samples is one second, which is 25 frames.
    CHECK(scheduler.positionAt(48000) == at(125));
    CHECK(scheduler.positionAt(1920) == at(101));

    SECTION("at double speed the timeline advances twice as fast") {
        scheduler.start(at(0), time::Rational::fromInt(2), 0);
        CHECK(scheduler.positionAt(48000) == at(50));
    }

    SECTION("at a quarter speed, exactly") {
        scheduler.start(at(0), time::Rational{1, 4}, 0);
        CHECK(scheduler.positionAt(48000) == at(6));  // 6.25 frames, truncated to 6
        CHECK(scheduler.positionAt(4 * 48000) == at(25));
    }

    SECTION("in reverse the playhead moves backwards") {
        scheduler.start(at(100), time::Rational::fromInt(-1), 0);
        CHECK(scheduler.positionAt(48000) == at(75));
    }
}

TEST_CASE("29.97 does not drift over an hour", "[playback][scheduler]") {
    // The rate that punishes floating point. An hour of audio must land on
    // exactly the frame timecode says it should.
    PlaybackScheduler::Config config;
    config.frameRate = time::rates::fps29_97;
    config.audioRate = time::rates::hz48000;
    config.duration = time::RationalTime{200000, time::rates::fps29_97};
    PlaybackScheduler scheduler{config};
    scheduler.start(at(0, time::rates::fps29_97), time::Rational::fromInt(1), 0);

    // One hour of audio.
    const std::int64_t oneHourOfSamples = 48000LL * 3600LL;
    const time::RationalTime position = scheduler.positionAt(oneHourOfSamples);
    // 3600s at 30000/1001 fps is exactly 107892.107... frames.
    CHECK(position.frames() == 107892);
    CHECK(position.rate() == time::rates::fps29_97);
}

TEST_CASE("The renderer is asked for frames in order, up to the queue depth",
          "[playback][scheduler]") {
    PlaybackScheduler scheduler{config25(3)};
    scheduler.start(at(0), time::Rational::fromInt(1), 0);

    CHECK(*scheduler.nextRenderTarget() == at(0));
    scheduler.submit(at(0), tiny());
    CHECK(*scheduler.nextRenderTarget() == at(1));
    scheduler.submit(at(1), tiny());
    scheduler.submit(at(2), tiny());

    CHECK(scheduler.queued() == 3);
    CHECK_FALSE(scheduler.nextRenderTarget().has_value());
}

TEST_CASE("The frame under the playhead is the one presented", "[playback][scheduler]") {
    PlaybackScheduler scheduler{config25()};
    scheduler.start(at(0), time::Rational::fromInt(1), 0);
    renderAhead(scheduler);

    const auto first = scheduler.present(0);
    CHECK(first.action == PresentAction::Present);
    CHECK(first.shown == at(0));
    CHECK(first.image != nullptr);

    // Not yet a whole frame later: keep showing frame 0.
    const auto held = scheduler.present(1000);
    CHECK(held.action == PresentAction::Repeat);
    CHECK(held.shown == at(0));

    // 1920 samples is exactly one frame at 25fps.
    const auto second = scheduler.present(1920);
    CHECK(second.action == PresentAction::Present);
    CHECK(second.shown == at(1));
    CHECK(second.dropped == 0);
}

TEST_CASE("Frames the clock has already passed are dropped, not shown late",
          "[playback][scheduler]") {
    PlaybackScheduler scheduler{config25()};
    scheduler.start(at(0), time::Rational::fromInt(1), 0);
    renderAhead(scheduler);

    // Jump the clock four frames ahead in one go.
    const auto result = scheduler.present(4 * 1920);
    CHECK(result.action == PresentAction::Present);
    CHECK(result.shown == at(4));
    CHECK(result.dropped == 4);
    CHECK(scheduler.stats().dropped == 4);
}

TEST_CASE("Falling behind advances the render head past the backlog", "[playback][scheduler]") {
    // A renderer that works through its backlog in order never catches up.
    PlaybackScheduler scheduler{config25()};
    scheduler.start(at(0), time::Rational::fromInt(1), 0);
    renderAhead(scheduler);
    REQUIRE(scheduler.renderHead() == at(6));

    scheduler.present(100 * 1920);
    // The head has jumped to just past the playhead rather than continuing
    // from where it was.
    CHECK(scheduler.renderHead() >= at(100));
}

TEST_CASE("An empty queue starves rather than showing nothing", "[playback][scheduler]") {
    PlaybackScheduler scheduler{config25()};
    scheduler.start(at(0), time::Rational::fromInt(1), 0);

    const auto result = scheduler.present(0);
    CHECK(result.action == PresentAction::Starve);
    CHECK(scheduler.stats().starved == 1);
}

TEST_CASE("A frame rendered for a time already passed is discarded", "[playback][scheduler]") {
    PlaybackScheduler scheduler{config25()};
    scheduler.start(at(0), time::Rational::fromInt(1), 0);
    renderAhead(scheduler);
    scheduler.present(50 * 1920);

    const std::size_t before = scheduler.queued();
    scheduler.submit(at(3), tiny());  // finished far too late
    CHECK(scheduler.queued() == before);
    CHECK(scheduler.stats().submittedTooLate == 1);
}

TEST_CASE("Seeking throws away work for the wrong time", "[playback][scheduler]") {
    PlaybackScheduler scheduler{config25()};
    scheduler.start(at(0), time::Rational::fromInt(1), 0);
    renderAhead(scheduler);
    REQUIRE(scheduler.queued() > 0);

    scheduler.seek(at(500), 1000);
    CHECK(scheduler.queued() == 0);
    CHECK(scheduler.positionAt(1000) == at(500));
    CHECK(scheduler.renderHead() == at(500));
}

TEST_CASE("Changing speed keeps the playhead continuous", "[playback][scheduler]") {
    PlaybackScheduler scheduler{config25()};
    scheduler.start(at(0), time::Rational::fromInt(1), 0);

    const time::RationalTime before = scheduler.positionAt(48000);
    REQUIRE(before == at(25));

    scheduler.setSpeed(time::Rational::fromInt(4), 48000);
    // The playhead does not jump at the moment of the change...
    CHECK(scheduler.positionAt(48000) == at(25));
    // ...and moves four times as fast afterwards.
    CHECK(scheduler.positionAt(2 * 48000) == at(125));
}

TEST_CASE("Reversing direction discards the queue", "[playback][scheduler]") {
    PlaybackScheduler scheduler{config25()};
    scheduler.start(at(100), time::Rational::fromInt(1), 0);
    renderAhead(scheduler);
    REQUIRE(scheduler.queued() > 0);

    scheduler.setSpeed(time::Rational::fromInt(-1), 0);
    // Everything queued was ahead in the old direction, which is behind now.
    CHECK(scheduler.queued() == 0);
    CHECK(*scheduler.nextRenderTarget() == at(100));

    scheduler.submit(at(100), tiny());
    CHECK(*scheduler.nextRenderTarget() == at(99));
}

TEST_CASE("Playback stops at the end of the sequence", "[playback][scheduler]") {
    PlaybackScheduler scheduler{config25(6, 10)};
    scheduler.start(at(8), time::Rational::fromInt(1), 0);

    CHECK(*scheduler.nextRenderTarget() == at(8));
    scheduler.submit(at(8), tiny());
    scheduler.submit(at(9), tiny());
    CHECK_FALSE(scheduler.nextRenderTarget().has_value());
}

TEST_CASE("Ten minutes of playback at 59.94 stays in sync", "[playback][scheduler][simulation]") {
    // The Phase 3 exit criterion, simulated. A real ten-minute test would take
    // ten minutes and could not induce the conditions that matter; this runs
    // the same scheduling decisions deterministically in milliseconds, with the
    // renderer deliberately unable to keep up for part of it.
    PlaybackScheduler::Config config;
    config.frameRate = time::rates::fps59_94;
    config.audioRate = time::rates::hz48000;
    config.queueCapacity = 8;
    config.duration = time::RationalTime{40000, time::rates::fps59_94};
    PlaybackScheduler scheduler{config};
    scheduler.start(at(0, time::rates::fps59_94), time::Rational::fromInt(1), 0);

    // A 60Hz display: 800 samples of audio between refreshes at 48kHz.
    constexpr std::int64_t kSamplesPerTick = 800;
    constexpr std::int64_t kTicks = 36000;  // ten minutes

    std::int64_t clock = 0;
    std::int64_t worstHealthyOffset = 0;
    std::int64_t starvedWhileHealthy = 0;
    std::int64_t ticksToRecover = 0;
    bool recovering = false;

    for (std::int64_t tick = 0; tick < kTicks; ++tick) {
        // The renderer manages two frames per refresh normally, which is ahead
        // of the 59.94 it needs. Between ticks 12000 and 12600 it manages none
        // at all -- a hitch of ten seconds, far worse than anything real.
        const bool stalled = tick >= 12000 && tick < 12600;
        const int budget = stalled ? 0 : 2;

        for (int i = 0; i < budget; ++i) {
            const auto target = scheduler.nextRenderTarget();
            if (!target) {
                break;
            }
            scheduler.submit(*target, tiny());
        }

        clock += kSamplesPerTick;
        const auto result = scheduler.present(clock);

        if (stalled) {
            recovering = true;
            continue;
        }
        if (recovering) {
            ++ticksToRecover;
            if (result.action == PresentAction::Present && result.dropped == 0) {
                recovering = false;
            }
            continue;
        }

        // Healthy: the frame on screen must be the one the clock is inside.
        const time::RationalTime position = scheduler.positionAt(clock);
        const std::int64_t offset =
            std::abs((position - result.shown).rescaledTo(config.frameRate).frames());
        worstHealthyOffset = std::max(worstHealthyOffset, offset);
        if (result.action == PresentAction::Starve) {
            ++starvedWhileHealthy;
        }
    }

    INFO("presented " << scheduler.stats().presented << ", dropped " << scheduler.stats().dropped
                      << ", starved " << scheduler.stats().starved << ", recovery took "
                      << ticksToRecover << " ticks");

    // Sync holds whenever the renderer can keep up.
    CHECK(worstHealthyOffset <= 1);
    CHECK(starvedWhileHealthy == 0);
    // And a ten-second stall is recovered from promptly, rather than leaving
    // playback permanently behind.
    CHECK(ticksToRecover < 60);

    // The stall showed up as starvation -- nothing to present -- which is the
    // honest outcome when the renderer produces nothing at all.
    CHECK(scheduler.stats().starved > 500);

    // And nothing was rendered only to be thrown away. The catch-up policy
    // moves the render head past the backlog instead of queueing frames that
    // are already late, so the renderer never spends its budget on work that
    // cannot be shown. A scheduler without that policy would drop hundreds
    // here and take far longer to recover.
    CHECK(scheduler.stats().dropped == 0);
    CHECK(scheduler.stats().submittedTooLate == 0);
    CHECK(scheduler.stats().presented > 30000);
}

TEST_CASE("A slow renderer drops picture but never loses the clock",
          "[playback][scheduler][simulation]") {
    // Half the frame rate available, sustained. Playback should run at the
    // right speed showing every other frame, not run slow showing all of them.
    PlaybackScheduler::Config config;
    config.frameRate = time::rates::fps25;
    config.audioRate = time::rates::hz48000;
    config.queueCapacity = 4;
    config.duration = time::RationalTime{10000, time::rates::fps25};
    PlaybackScheduler scheduler{config};
    scheduler.start(at(0), time::Rational::fromInt(1), 0);

    std::int64_t clock = 0;
    std::int64_t worstOffset = 0;
    for (std::int64_t tick = 0; tick < 5000; ++tick) {
        // One frame rendered per two frames of elapsed time.
        if (tick % 2 == 0) {
            if (const auto target = scheduler.nextRenderTarget()) {
                scheduler.submit(*target, tiny());
            }
        }
        clock += 1920;  // one frame of audio per tick
        const auto result = scheduler.present(clock);
        if (result.action == PresentAction::Present) {
            const std::int64_t offset =
                std::abs((scheduler.positionAt(clock) - result.shown).frames());
            worstOffset = std::max(worstOffset, offset);
        }
    }

    INFO("worst offset " << worstOffset << " frames, presented " << scheduler.stats().presented
                         << ", dropped " << scheduler.stats().dropped);
    // Whatever is shown is close to the playhead: the timeline never falls
    // behind the clock, it just shows fewer frames.
    CHECK(worstOffset <= 2);
    CHECK(scheduler.stats().presented > 2000);
    // Half the frames never make it to the screen, and that is the correct
    // outcome. Playback runs at the right speed showing every other frame,
    // rather than at half speed showing all of them.
    CHECK(scheduler.stats().presented < 3000);
}
