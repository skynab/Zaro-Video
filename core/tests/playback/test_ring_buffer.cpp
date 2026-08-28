#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/playback/AudioRingBuffer.h"

using zaro::playback::AudioRingBuffer;

namespace {

std::vector<float> ramp(std::int64_t frames, std::int32_t channels, float start = 0.0F) {
    std::vector<float> out(static_cast<std::size_t>(frames * channels));
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = start + static_cast<float>(i);
    }
    return out;
}

}  // namespace

TEST_CASE("Samples come out in the order they went in", "[playback][ring]") {
    AudioRingBuffer ring{2, 128};
    const std::vector<float> input = ramp(64, 2);

    CHECK(ring.write(input.data(), 64) == 64);
    CHECK(ring.availableToRead() == 64);

    std::vector<float> output(128, -1.0F);
    CHECK(ring.read(output.data(), 64) == 64);
    for (std::size_t i = 0; i < input.size(); ++i) {
        REQUIRE(output[i] == input[i]);
    }
    CHECK(ring.availableToRead() == 0);
}

TEST_CASE("Writing past capacity writes only what fits", "[playback][ring]") {
    AudioRingBuffer ring{1, 16};
    const std::vector<float> input = ramp(32, 1);
    CHECK(ring.write(input.data(), 32) == 16);
    CHECK(ring.availableToWrite() == 0);
}

TEST_CASE("Reading an empty buffer yields silence, not stale samples", "[playback][ring]") {
    // An underrun should be a gap. Replaying whatever was last in memory turns
    // a dropout into a stutter, which sounds like a bug rather than a glitch.
    AudioRingBuffer ring{2, 64};
    const std::vector<float> input = ramp(32, 2, 1.0F);
    ring.write(input.data(), 32);

    std::vector<float> output(64 * 2, -1.0F);
    CHECK(ring.read(output.data(), 64) == 32);
    for (std::size_t i = 64; i < output.size(); ++i) {
        REQUIRE(output[i] == 0.0F);
    }
    CHECK(ring.underrunFrames() == 32);
}

TEST_CASE("The read position never overtakes the write position", "[playback][ring]") {
    // The bug this guards: advancing the read position for invented silence
    // runs it past the write position, availableToRead goes negative, and the
    // next read memsets with a negative count.
    AudioRingBuffer ring{1, 64};
    std::vector<float> output(100, 0.0F);
    for (int round = 0; round < 5; ++round) {
        ring.read(output.data(), 100);
        REQUIRE(ring.availableToRead() >= 0);
        REQUIRE(ring.framesConsumed() <= ring.framesWritten());
    }
    const std::vector<float> input = ramp(32, 1, 1.0F);
    CHECK(ring.write(input.data(), 32) == 32);
    CHECK(ring.read(output.data(), 32) == 32);
    CHECK(output[0] == 1.0F);
}

TEST_CASE("The clock counts everything the device consumed", "[playback][ring]") {
    // Including silence invented during an underrun: that time really did pass,
    // and a clock that stalls exactly when playback is in trouble is worse than
    // useless.
    AudioRingBuffer ring{1, 64};
    std::vector<float> output(100, 0.0F);
    ring.read(output.data(), 100);
    CHECK(ring.framesDelivered() == 100);
    CHECK(ring.framesConsumed() == 0);
    CHECK(ring.underrunFrames() == 100);
}

TEST_CASE("The buffer wraps correctly", "[playback][ring]") {
    AudioRingBuffer ring{1, 10};
    std::vector<float> scratch(10, 0.0F);

    float next = 0.0F;
    for (int round = 0; round < 20; ++round) {
        std::vector<float> input(7);
        for (float& sample : input) {
            sample = next++;
        }
        REQUIRE(ring.write(input.data(), 7) == 7);
        REQUIRE(ring.read(scratch.data(), 7) == 7);
        for (int i = 0; i < 7; ++i) {
            REQUIRE(scratch[static_cast<std::size_t>(i)] == input[static_cast<std::size_t>(i)]);
        }
    }
    CHECK(ring.framesDelivered() == 140);
    CHECK(ring.framesConsumed() == 140);
}

TEST_CASE("A producer and a consumer on separate threads agree", "[playback][ring]") {
    // The real arrangement: a render thread filling, an audio callback
    // draining, and no lock between them.
    constexpr std::int64_t kTotal = 200000;
    AudioRingBuffer ring{1, 1024};

    std::thread producer{[&] {
        std::int64_t written = 0;
        std::vector<float> block(256);
        while (written < kTotal) {
            const std::int64_t want = std::min<std::int64_t>(256, kTotal - written);
            for (std::int64_t i = 0; i < want; ++i) {
                block[static_cast<std::size_t>(i)] = static_cast<float>(written + i);
            }
            std::int64_t done = 0;
            while (done < want) {
                done += ring.write(block.data() + done, want - done);
            }
            written += want;
        }
    }};

    std::int64_t read = 0;
    std::int64_t mismatches = 0;
    std::vector<float> block(128);
    while (read < kTotal) {
        const std::int64_t got = ring.read(block.data(), 128);
        for (std::int64_t i = 0; i < got; ++i) {
            if (block[static_cast<std::size_t>(i)] != static_cast<float>(read + i)) {
                ++mismatches;
            }
        }
        read += got;
    }
    producer.join();

    CHECK(mismatches == 0);
    CHECK(read == kTotal);
}

TEST_CASE("Reset clears the clock and the contents", "[playback][ring]") {
    AudioRingBuffer ring{1, 32};
    const std::vector<float> input = ramp(16, 1, 1.0F);
    ring.write(input.data(), 16);
    ring.reset();
    CHECK(ring.framesDelivered() == 0);
    CHECK(ring.framesWritten() == 0);
    CHECK(ring.availableToRead() == 0);
}
