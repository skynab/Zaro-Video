#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/media/Waveform.h"

using namespace zaro;
using Catch::Approx;
using media::AudioBuffer;
using media::Waveform;

namespace {

/// A block of a sine, so minima and maxima are known and symmetric.
AudioBuffer sine(std::int64_t samples, std::int32_t channels, float amplitude,
                 std::int64_t phase = 0) {
    AudioBuffer block{channels, samples, time::rates::hz48000};
    for (std::int32_t c = 0; c < channels; ++c) {
        for (std::int64_t i = 0; i < samples; ++i) {
            const double angle = 2.0 * std::acos(-1.0) * static_cast<double>(phase + i) / 64.0;
            block.channel(c)[i] = amplitude * static_cast<float>(std::sin(angle));
        }
    }
    return block;
}

}  // namespace

TEST_CASE("A waveform records the envelope, not the average", "[media][waveform]") {
    // Averaging a symmetric signal gives zero everywhere and draws a flat line.
    Waveform waveform{1, 64, time::rates::hz48000};
    waveform.append(sine(640, 1, 0.8F));
    waveform.finish();

    REQUIRE(waveform.bucketCount() == 10);
    for (std::int64_t b = 0; b < waveform.bucketCount(); ++b) {
        INFO("bucket " << b);
        CHECK(waveform.at(0, b).maximum == Approx(0.8F).margin(0.02));
        CHECK(waveform.at(0, b).minimum == Approx(-0.8F).margin(0.02));
    }
}

TEST_CASE("Block boundaries do not show up in the result", "[media][waveform]") {
    // The same audio delivered as one long read and as many short ones must
    // produce identical buckets, or a waveform would change shape depending on
    // how the decoder happened to chunk its output.
    Waveform whole{2, 100, time::rates::hz48000};
    whole.append(sine(1000, 2, 0.5F));
    whole.finish();

    Waveform pieced{2, 100, time::rates::hz48000};
    std::int64_t written = 0;
    for (const std::int64_t chunk : {7, 130, 3, 260, 41, 559}) {
        pieced.append(sine(chunk, 2, 0.5F, written));
        written += chunk;
    }
    pieced.finish();

    REQUIRE(whole.bucketCount() == pieced.bucketCount());
    for (std::int64_t b = 0; b < whole.bucketCount(); ++b) {
        for (std::int32_t c = 0; c < 2; ++c) {
            INFO("channel " << c << " bucket " << b);
            CHECK(whole.at(c, b).minimum == Approx(pieced.at(c, b).minimum).margin(1e-6));
            CHECK(whole.at(c, b).maximum == Approx(pieced.at(c, b).maximum).margin(1e-6));
        }
    }
}

TEST_CASE("A partial final bucket is kept", "[media][waveform]") {
    Waveform waveform{1, 100, time::rates::hz48000};
    waveform.append(sine(250, 1, 1.0F));
    waveform.finish();
    // Two full buckets and the remaining fifty samples.
    CHECK(waveform.bucketCount() == 3);
}

TEST_CASE("Channels are kept apart", "[media][waveform]") {
    Waveform waveform{2, 50, time::rates::hz48000};
    AudioBuffer block{2, 100, time::rates::hz48000};
    for (std::int64_t i = 0; i < 100; ++i) {
        block.channel(0)[i] = 0.9F;
        block.channel(1)[i] = -0.3F;
    }
    waveform.append(block);
    waveform.finish();

    CHECK(waveform.at(0, 0).maximum == Approx(0.9F));
    CHECK(waveform.at(1, 0).maximum == Approx(-0.3F));
    CHECK(waveform.at(1, 0).minimum == Approx(-0.3F));
}

TEST_CASE("Resampling folds buckets without losing peaks", "[media][waveform]") {
    // Drawing asks for one bucket per pixel. A peak that survives at full
    // resolution has to survive the reduction, or transients vanish as you zoom
    // out -- which is exactly when you are looking for them.
    Waveform waveform{1, 10, time::rates::hz48000};
    AudioBuffer block{1, 1000, time::rates::hz48000};
    for (std::int64_t i = 0; i < 1000; ++i) {
        block.channel(0)[i] = 0.1F;
    }
    block.channel(0)[543] = 0.95F;  // a single transient
    waveform.append(block);
    waveform.finish();
    REQUIRE(waveform.bucketCount() == 100);

    const Waveform reduced = waveform.resampled(0, 100, 10);
    CHECK(reduced.bucketCount() == 10);

    float loudest = 0.0F;
    for (std::int64_t b = 0; b < reduced.bucketCount(); ++b) {
        loudest = std::max(loudest, reduced.at(0, b).maximum);
    }
    CHECK(loudest == Approx(0.95F));

    SECTION("and a sub-range reduces only that range") {
        const Waveform tail = waveform.resampled(60, 100, 4);
        CHECK(tail.bucketCount() == 4);
        float peak = 0.0F;
        for (std::int64_t b = 0; b < tail.bucketCount(); ++b) {
            peak = std::max(peak, tail.at(0, b).maximum);
        }
        // The transient is at bucket 54, outside this range.
        CHECK(peak == Approx(0.1F));
    }
}

TEST_CASE("A waveform survives a round trip through its cache format", "[media][waveform]") {
    Waveform waveform{2, 128, time::rates::hz44100};
    waveform.append(sine(2000, 2, 0.7F));
    waveform.finish();

    const std::string encoded = waveform.encode();
    const auto decoded = Waveform::decode(encoded);
    REQUIRE(decoded);

    CHECK(decoded->channelCount() == 2);
    CHECK(decoded->samplesPerBucket() == 128);
    CHECK(decoded->sampleRate() == time::rates::hz44100);
    CHECK(decoded->bucketCount() == waveform.bucketCount());
    for (std::int64_t b = 0; b < waveform.bucketCount(); ++b) {
        REQUIRE(decoded->at(0, b) == waveform.at(0, b));
        REQUIRE(decoded->at(1, b) == waveform.at(1, b));
    }
}

TEST_CASE("A corrupt cache file is rejected rather than misread", "[media][waveform]") {
    CHECK_FALSE(Waveform::decode("").hasValue());
    CHECK_FALSE(Waveform::decode("not a waveform at all").hasValue());

    Waveform waveform{1, 64, time::rates::hz48000};
    waveform.append(sine(640, 1, 0.5F));
    waveform.finish();
    const std::string encoded = waveform.encode();

    SECTION("truncated") {
        CHECK_FALSE(Waveform::decode(encoded.substr(0, encoded.size() / 2)).hasValue());
    }
    SECTION("wrong magic") {
        std::string damaged = encoded;
        damaged[0] = 'X';
        CHECK_FALSE(Waveform::decode(damaged).hasValue());
    }
}

TEST_CASE("The quick content hash distinguishes files", "[media][waveform][hash]") {
    const std::string a = std::string{ZARO_SCRATCH_DIR} + "/hash-a.bin";
    const std::string b = std::string{ZARO_SCRATCH_DIR} + "/hash-b.bin";
    {
        std::ofstream out{a, std::ios::binary};
        out << std::string(200000, 'a');
    }
    {
        std::ofstream out{b, std::ios::binary};
        out << std::string(200000, 'b');
    }

    const auto hashA = media::quickContentHash(a);
    const auto hashB = media::quickContentHash(b);
    REQUIRE(hashA);
    REQUIRE(hashB);
    CHECK(*hashA != *hashB);

    SECTION("and is stable for the same file") {
        CHECK(*media::quickContentHash(a) == *hashA);
    }

    SECTION("a missing file is an error, not an empty hash") {
        CHECK_FALSE(media::quickContentHash(a + ".missing").hasValue());
    }
}
