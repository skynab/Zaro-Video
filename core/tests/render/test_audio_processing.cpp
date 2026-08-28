#include <cmath>
#include <cstdint>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/AudioProcessor.h"

using namespace zaro;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

/// The amplitude that comes out when a sine at this frequency goes in.
///
/// Measured after letting the filter settle, because a biquad's first few
/// samples are its transient and every filter has one.
double responseAt(render::Biquad& filter, double frequencyHz) {
    filter.reset();
    const std::int64_t settle = 4096;
    const std::int64_t measure = 4096;
    double peak = 0.0;
    for (std::int64_t i = 0; i < settle + measure; ++i) {
        const auto phase =
            2.0 * 3.14159265358979323846 * frequencyHz * static_cast<double>(i) / kRate;
        const float out = filter.process(static_cast<float>(std::sin(phase)));
        if (i >= settle) {
            peak = std::max(peak, std::fabs(static_cast<double>(out)));
        }
    }
    return peak;
}

}  // namespace

TEST_CASE("A high pass removes what is below it and keeps what is above", "[render][audio][dsp]") {
    render::Biquad filter;
    filter.setHighPass(200.0, kRate);

    CHECK(responseAt(filter, 20.0) < 0.05);
    CHECK(responseAt(filter, 50.0) < 0.2);
    // At the corner a Butterworth section is 3 dB down, which is 0.707.
    CHECK(responseAt(filter, 200.0) == Approx(0.707).margin(0.05));
    CHECK(responseAt(filter, 2000.0) == Approx(1.0).margin(0.02));
}

TEST_CASE("A low pass is the mirror of it", "[render][audio][dsp]") {
    render::Biquad filter;
    filter.setLowPass(2000.0, kRate);

    CHECK(responseAt(filter, 200.0) == Approx(1.0).margin(0.02));
    CHECK(responseAt(filter, 2000.0) == Approx(0.707).margin(0.05));
    CHECK(responseAt(filter, 12000.0) < 0.1);
}

TEST_CASE("A bell lifts its own frequency and leaves the rest", "[render][audio][dsp]") {
    render::Biquad filter;
    filter.setPeaking(1000.0, kRate, 6.0, 1.0);

    // Six decibels is a factor of two.
    CHECK(responseAt(filter, 1000.0) == Approx(1.995).margin(0.05));
    CHECK(responseAt(filter, 100.0) == Approx(1.0).margin(0.05));
    CHECK(responseAt(filter, 12000.0) == Approx(1.0).margin(0.05));

    // And cuts as well as it boosts.
    filter.setPeaking(1000.0, kRate, -6.0, 1.0);
    CHECK(responseAt(filter, 1000.0) == Approx(0.501).margin(0.02));
}

TEST_CASE("A filter set to nothing is a bypass, not a near-bypass", "[render][audio][dsp]") {
    // Exactly unity matters: a chain of three sections that each nearly do
    // nothing still colours a track that was supposed to be untouched.
    render::Biquad filter;
    filter.setHighPass(0.0, kRate);
    CHECK(filter.isBypass());
    CHECK(filter.process(0.37F) == 0.37F);

    filter.setPeaking(1000.0, kRate, 0.0, 1.0);
    CHECK(filter.isBypass());

    // Above Nyquist there is nothing to filter.
    filter.setLowPass(30000.0, kRate);
    CHECK(filter.isBypass());
}

TEST_CASE("A compressor pulls down what is over the threshold", "[render][audio][dsp]") {
    model::Compressor compressor;
    compressor.enabled = true;
    compressor.thresholdDb = -20.0;
    compressor.ratio = 4.0;
    compressor.attackMs = 0.0;  // instant, so this measures the ratio and not the attack
    compressor.releaseMs = 0.0;

    render::TrackProcessor chain;
    chain.configure(model::AudioEq{}, compressor, kRate, 1);

    // A steady tone twenty decibels over: a quarter of that stays, so it ends
    // up five over.
    std::vector<float> samples(2048, 1.0F);
    float* channels[1] = {samples.data()};
    chain.process(channels, 1, static_cast<std::int64_t>(samples.size()));

    const double outDb = 20.0 * std::log10(std::fabs(static_cast<double>(samples.back())));
    CHECK(outDb == Approx(-15.0).margin(0.5));
    CHECK(chain.lastReductionDb() == Approx(-15.0).margin(0.5));
}

TEST_CASE("Below the threshold nothing happens at all", "[render][audio][dsp]") {
    model::Compressor compressor;
    compressor.enabled = true;
    compressor.thresholdDb = -6.0;
    compressor.ratio = 8.0;

    render::TrackProcessor chain;
    chain.configure(model::AudioEq{}, compressor, kRate, 1);

    std::vector<float> samples(512, 0.1F);  // about -20 dB
    float* channels[1] = {samples.data()};
    chain.process(channels, 1, static_cast<std::int64_t>(samples.size()));

    CHECK(samples.back() == Approx(0.1F).margin(1e-5));
    CHECK(chain.lastReductionDb() == Approx(0.0F));
}

TEST_CASE("The detector is shared across channels", "[render][audio][dsp]") {
    // Compressing each side separately makes the image wander whenever one of
    // them alone is loud, which is what makes a mix sound unstable rather than
    // controlled.
    model::Compressor compressor;
    compressor.enabled = true;
    compressor.thresholdDb = -20.0;
    compressor.ratio = 4.0;
    compressor.attackMs = 0.0;
    compressor.releaseMs = 0.0;

    render::TrackProcessor chain;
    chain.configure(model::AudioEq{}, compressor, kRate, 2);

    std::vector<float> left(1024, 1.0F);
    std::vector<float> right(1024, 0.05F);  // quiet side, well under the threshold
    float* channels[2] = {left.data(), right.data()};
    chain.process(channels, 2, 1024);

    // Both sides moved by the same amount, so their ratio is unchanged.
    CHECK(left.back() / right.back() == Approx(1.0F / 0.05F).epsilon(0.01));
}

TEST_CASE("Attack and release take the time they are given", "[render][audio][dsp]") {
    model::Compressor compressor;
    compressor.enabled = true;
    compressor.thresholdDb = -20.0;
    compressor.ratio = 8.0;
    compressor.attackMs = 20.0;
    compressor.releaseMs = 200.0;

    render::TrackProcessor chain;
    chain.configure(model::AudioEq{}, compressor, kRate, 1);

    // A loud burst. The first sample should be barely touched -- that is what
    // an attack time means -- and the level should keep falling through it.
    std::vector<float> samples(static_cast<std::size_t>(kRate / 10), 1.0F);
    float* channels[1] = {samples.data()};
    chain.process(channels, 1, static_cast<std::int64_t>(samples.size()));

    // The shape, not one instant: with a 20 ms time constant the envelope has
    // barely moved after a millisecond, so a threshold crossing there says
    // nothing. What an attack time means is that the reduction deepens over
    // time, and that is what this checks.
    const auto at = [&](double milliseconds) {
        return samples[static_cast<std::size_t>(kRate * milliseconds / 1000.0)];
    };
    CHECK(samples.front() == Approx(1.0F).margin(0.05));
    CHECK(at(1.0) > at(5.0));
    CHECK(at(5.0) > at(20.0));
    CHECK(at(20.0) > at(80.0));
    // And it has actually done something by the end.
    CHECK(samples.back() < 0.5F);
}

TEST_CASE("Resetting forgets the past", "[render][audio][dsp]") {
    // A seek has to reset the chain, or a compressor's envelope follows the
    // playhead from one part of the timeline to another and the first moment
    // after a jump is ducked for no reason.
    model::Compressor compressor;
    compressor.enabled = true;
    compressor.thresholdDb = -30.0;
    compressor.ratio = 10.0;
    compressor.attackMs = 0.0;
    compressor.releaseMs = 500.0;

    render::TrackProcessor chain;
    chain.configure(model::AudioEq{}, compressor, kRate, 1);

    std::vector<float> loud(4096, 1.0F);
    float* channels[1] = {loud.data()};
    chain.process(channels, 1, 4096);
    REQUIRE(chain.lastReductionDb() < -10.0F);

    // Quiet material immediately after is still ducked, because the envelope
    // has not released yet -- which is correct.
    std::vector<float> quiet(64, 0.02F);
    float* quietChannels[1] = {quiet.data()};
    chain.process(quietChannels, 1, 64);
    CHECK(quiet.front() < 0.02F);

    // After a reset it is not.
    chain.reset();
    std::vector<float> again(64, 0.02F);
    float* againChannels[1] = {again.data()};
    chain.process(againChannels, 1, 64);
    CHECK(again.front() == Approx(0.02F).margin(1e-4));
}

TEST_CASE("Makeup gain is applied after the reduction", "[render][audio][dsp]") {
    model::Compressor compressor;
    compressor.enabled = true;
    compressor.thresholdDb = -20.0;
    compressor.ratio = 4.0;
    compressor.attackMs = 0.0;
    compressor.releaseMs = 0.0;
    compressor.makeupDb = 6.0;

    render::TrackProcessor chain;
    chain.configure(model::AudioEq{}, compressor, kRate, 1);
    std::vector<float> samples(2048, 1.0F);
    float* channels[1] = {samples.data()};
    chain.process(channels, 1, 2048);

    // -15 dB from the compressor, then six back.
    const double outDb = 20.0 * std::log10(std::fabs(static_cast<double>(samples.back())));
    CHECK(outDb == Approx(-9.0).margin(0.5));
}
