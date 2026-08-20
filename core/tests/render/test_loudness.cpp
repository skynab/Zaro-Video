#include <cmath>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/Loudness.h"

using namespace zaro;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

/// A stereo sine of a given level, for a given number of seconds.
void feedSine(render::LoudnessMeter& meter, double dbfs, double seconds,
              double frequencyHz = 1000.0) {
    const auto samples = static_cast<std::int64_t>(kRate * seconds);
    const double amplitude = std::pow(10.0, dbfs / 20.0);
    std::vector<float> left(static_cast<std::size_t>(samples));
    std::vector<float> right(static_cast<std::size_t>(samples));
    for (std::int64_t i = 0; i < samples; ++i) {
        const auto value = static_cast<float>(
            amplitude * std::sin(2.0 * kPi * frequencyHz * static_cast<double>(i) / kRate));
        left[static_cast<std::size_t>(i)] = value;
        right[static_cast<std::size_t>(i)] = value;
    }
    const float* channels[2] = {left.data(), right.data()};
    meter.feed(channels, 2, samples);
}

void feedSilence(render::LoudnessMeter& meter, double seconds) {
    const auto samples = static_cast<std::int64_t>(kRate * seconds);
    std::vector<float> left(static_cast<std::size_t>(samples), 0.0F);
    std::vector<float> right(static_cast<std::size_t>(samples), 0.0F);
    const float* channels[2] = {left.data(), right.data()};
    meter.feed(channels, 2, samples);
}

}  // namespace

TEST_CASE("A 1 kHz sine reads its own level in LUFS", "[render][loudness]") {
    // The standard's own calibration case, and the reason for the -0.691
    // offset: a stereo 1 kHz sine at -23 dBFS is -23 LUFS by definition.
    render::LoudnessMeter meter;
    meter.configure(kRate, 2);
    feedSine(meter, -23.0, 5.0);

    CHECK(meter.integratedLufs() == Approx(-23.0).margin(0.15));
    CHECK(meter.shortTermLufs() == Approx(-23.0).margin(0.15));
    CHECK(meter.momentaryLufs() == Approx(-23.0).margin(0.15));
}

TEST_CASE("Level changes move the reading by the same amount", "[render][loudness]") {
    for (const double level : {-33.0, -23.0, -13.0}) {
        render::LoudnessMeter meter;
        meter.configure(kRate, 2);
        feedSine(meter, level, 4.0);
        INFO("level " << level);
        CHECK(meter.integratedLufs() == Approx(level).margin(0.2));
    }
}

TEST_CASE("K-weighting lifts the top and cuts the bottom", "[render][loudness]") {
    // The weighting is the whole point: a rumble at 30 Hz is not as loud as a
    // tone at 3 kHz, and a meter that said otherwise would have everyone
    // filtering their programmes to pass it.
    const auto measure = [](double frequency) {
        render::LoudnessMeter meter;
        meter.configure(kRate, 2);
        feedSine(meter, -23.0, 4.0, frequency);
        return meter.integratedLufs();
    };

    // The figures are the standard's, not a target: its high pass sits at
    // 38 Hz with a Q of 0.5, which puts 30 Hz about nine decibels below a
    // kilohertz, and its shelf is four decibels up by six.
    CHECK(measure(30.0) < measure(1000.0) - 8.0);
    CHECK(measure(6000.0) > measure(1000.0) + 2.0);
}

TEST_CASE("Silence measures as silence rather than as minus infinity", "[render][loudness]") {
    render::LoudnessMeter meter;
    meter.configure(kRate, 2);
    feedSilence(meter, 2.0);
    CHECK(meter.integratedLufs() == Approx(render::LoudnessMeter::kSilence));
    CHECK(meter.momentaryLufs() == Approx(render::LoudnessMeter::kSilence));
    CHECK(meter.samplePeakDbfs() < -100.0);
}

TEST_CASE("Quiet passages do not drag the integrated reading down", "[render][loudness]") {
    // The gate. Without it a programme with pauses in it measures quieter than
    // it sounds, so everyone pushes the loud parts up to compensate -- which is
    // the loudness war moving rather than ending.
    render::LoudnessMeter gated;
    gated.configure(kRate, 2);
    feedSine(gated, -23.0, 4.0);
    feedSilence(gated, 6.0);
    feedSine(gated, -23.0, 4.0);

    render::LoudnessMeter continuous;
    continuous.configure(kRate, 2);
    feedSine(continuous, -23.0, 8.0);

    // Six seconds of silence in the middle of it, and the answer is the same.
    CHECK(gated.integratedLufs() == Approx(continuous.integratedLufs()).margin(0.3));
}

TEST_CASE("A very quiet passage is gated out too", "[render][loudness]") {
    // The relative gate catches material that is programme by the absolute
    // measure but far below the rest of the mix.
    render::LoudnessMeter meter;
    meter.configure(kRate, 2);
    feedSine(meter, -20.0, 4.0);
    feedSine(meter, -50.0, 8.0);  // twice as long, thirty decibels down

    // Dominated by the loud part rather than averaged with the quiet one.
    CHECK(meter.integratedLufs() == Approx(-20.0).margin(0.5));
}

TEST_CASE("Momentary follows the recent past, integrated does not", "[render][loudness]") {
    render::LoudnessMeter meter;
    meter.configure(kRate, 2);
    feedSine(meter, -30.0, 4.0);
    const double afterQuiet = meter.momentaryLufs();
    feedSine(meter, -14.0, 1.0);

    CHECK(afterQuiet == Approx(-30.0).margin(0.3));
    // The last 400 ms is the loud part.
    CHECK(meter.momentaryLufs() == Approx(-14.0).margin(0.3));
    // The integrated figure covers both, so it sits between them.
    CHECK(meter.integratedLufs() > -30.0);
    CHECK(meter.integratedLufs() < -14.0);
}

TEST_CASE("Sample peak is the largest sample, and says so", "[render][loudness]") {
    // Not true peak: inter-sample peaks need oversampling, and claiming true
    // peak without it would be a number that passes a check the delivered file
    // fails.
    render::LoudnessMeter meter;
    meter.configure(kRate, 2);
    feedSine(meter, -6.0, 1.0);
    CHECK(meter.samplePeakDbfs() == Approx(-6.0).margin(0.1));
}

TEST_CASE("A reset forgets everything", "[render][loudness]") {
    render::LoudnessMeter meter;
    meter.configure(kRate, 2);
    feedSine(meter, -10.0, 3.0);
    REQUIRE(meter.integratedLufs() > -12.0);

    meter.reset();
    CHECK(meter.integratedLufs() == Approx(render::LoudnessMeter::kSilence));
    feedSine(meter, -30.0, 3.0);
    CHECK(meter.integratedLufs() == Approx(-30.0).margin(0.3));
}

TEST_CASE("The measurement does not depend on how it is fed", "[render][loudness]") {
    // Block size is a property of whatever is calling, not of the programme --
    // the same reasoning as the per-sample automation.
    const auto measureInBlocks = [](std::int64_t block) {
        render::LoudnessMeter meter;
        meter.configure(kRate, 2);
        const auto total = static_cast<std::int64_t>(kRate * 4.0);
        const double amplitude = std::pow(10.0, -23.0 / 20.0);
        std::vector<float> left(static_cast<std::size_t>(block));
        std::vector<float> right(static_cast<std::size_t>(block));
        std::int64_t done = 0;
        while (done < total) {
            const std::int64_t count = std::min(block, total - done);
            for (std::int64_t i = 0; i < count; ++i) {
                const auto value =
                    static_cast<float>(amplitude * std::sin(2.0 * kPi * 1000.0 *
                                                            static_cast<double>(done + i) / kRate));
                left[static_cast<std::size_t>(i)] = value;
                right[static_cast<std::size_t>(i)] = value;
            }
            const float* channels[2] = {left.data(), right.data()};
            meter.feed(channels, 2, count);
            done += count;
        }
        return meter.integratedLufs();
    };

    const double reference = measureInBlocks(48000);
    for (const std::int64_t block : {64, 512, 1024, 4800}) {
        INFO("block " << block);
        CHECK(measureInBlocks(block) == Approx(reference).margin(0.05));
    }
}
