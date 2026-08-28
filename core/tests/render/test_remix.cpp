#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/Remix.h"

using namespace zaro;
using Catch::Approx;

namespace {

/// A click track: a short burst every `beatSeconds`, silence between. The
/// beats are known exactly, so "did it find them" has an answer.
class ClickSource final : public render::AudioSource {
public:
    ClickSource(double beatSeconds, double seconds) : beat_{beatSeconds}, seconds_{seconds} {}

    Status read(model::MediaRefId, const time::RationalTime& sourceStart, std::int64_t sampleCount,
                const time::Rational& sampleRate, media::AudioBuffer& out) override {
        out = media::AudioBuffer{1, sampleCount, sampleRate};
        const double rate = sampleRate.toDouble();
        const double start = sourceStart.toSecondsDouble();
        float* samples = out.channel(0);
        for (std::int64_t i = 0; i < sampleCount; ++i) {
            const double at = start + (static_cast<double>(i) / rate);
            if (at >= seconds_) {
                samples[i] = 0.0F;
                continue;
            }
            const double intoBeat = std::fmod(at, beat_);
            // A 30 ms burst of tone on each beat.
            samples[i] =
                intoBeat < 0.03
                    ? static_cast<float>(0.8 * std::sin(at * 2.0 * std::numbers::pi * 440.0))
                    : 0.0F;
        }
        return {};
    }

private:
    double beat_;
    double seconds_;
};

/// Beats at a tempo that does not start at zero, and with two of them missing.
///
/// Not a tidy grid on purpose: with beats every half second from zero, an
/// arithmetic cut that ignored them entirely would still land on one, and the
/// test that says "both edges are beats" would pass against a version that had
/// never looked at the list.
std::vector<double> steadyBeats(double beat = 0.47, double seconds = 30.0) {
    std::vector<double> beats;
    int index = 0;
    for (double at = 0.13; at < seconds; at += beat, ++index) {
        if (index == 11 || index == 26) {
            continue;  // a bar where nothing lands
        }
        beats.push_back(at);
    }
    return beats;
}

}  // namespace

TEST_CASE("the beats of a click track are found where they are", "[remix]") {
    ClickSource source{0.5, 8.0};
    auto beats = render::detectBeats(source, model::MediaRefId{1}, 8.0, time::rates::hz48000);
    REQUIRE(beats);
    // Sixteen beats in eight seconds, give or take the first one and the last.
    CHECK(beats->size() >= 14);
    CHECK(beats->size() <= 17);
    for (const double at : *beats) {
        // Each one lands on a half second, within one analysis window.
        const double intoBeat = std::fmod(at + 0.005, 0.5);
        CHECK(std::min(intoBeat, 0.5 - intoBeat) < 0.03);
    }
}

TEST_CASE("silence has no beats in it", "[remix]") {
    ClickSource source{0.5, 0.0};  // never reaches a click
    auto beats = render::detectBeats(source, model::MediaRefId{1}, 4.0, time::rates::hz48000);
    CHECK_FALSE(beats);
}

TEST_CASE("a remix comes out close to the length asked for", "[remix]") {
    const auto plan = render::planRemix(steadyBeats(), 30.0, 20.0);
    REQUIRE(plan);
    // Within a beat: landing exactly would mean cutting off the beat, which is
    // the one thing this exists to avoid.
    CHECK(plan->seconds == Approx(20.0).margin(0.5));
    CHECK(plan->cutAt < plan->resumeFrom);
}

TEST_CASE("both edges of the cut land on beats", "[remix]") {
    const std::vector<double> beats = steadyBeats();
    const auto plan = render::planRemix(beats, 30.0, 17.0);
    REQUIRE(plan);

    const auto onABeat = [&beats](double when) {
        return std::any_of(beats.begin(), beats.end(),
                           [when](double beat) { return std::fabs(beat - when) < 1e-9; });
    };
    CHECK(onABeat(plan->cutAt));
    CHECK(onABeat(plan->resumeFrom));
    // And a whole number of beats comes out, so the music does not arrive on
    // the wrong foot afterwards.
    CHECK(plan->beatsRemoved > 0);
}

TEST_CASE("making music longer is refused rather than looped", "[remix]") {
    auto plan = render::planRemix(steadyBeats(), 30.0, 45.0);
    CHECK_FALSE(plan);
    CHECK(plan.error().message().find("different job") != std::string::npos);
}

TEST_CASE("a track with nothing to cut on is refused", "[remix]") {
    CHECK_FALSE(render::planRemix({}, 30.0, 20.0));
    CHECK_FALSE(render::planRemix({1.0, 2.0}, 30.0, 20.0));
}

TEST_CASE("the join fade fits inside both halves", "[remix]") {
    // A cut very near the start leaves almost nothing to fade from.
    const std::vector<double> beats{0.0, 0.02, 25.0, 29.0};
    const auto plan = render::planRemix(beats, 30.0, 6.0);
    REQUIRE(plan);
    CHECK(plan->joinFade >= 0.0);
    CHECK(plan->joinFade <= plan->cutAt);
    CHECK(plan->joinFade <= 30.0 - plan->resumeFrom);
}
