#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "zaro/platform/ffmpeg/FFmpegMedia.h"

#include "Fixtures.h"

using namespace zaro;
using zaro::testing::fixture;

namespace {

/// Decode everything and report total samples plus per-channel peak.
struct Drained {
    std::int64_t samples{0};
    std::vector<float> peaks;
    time::Rational rate;
};

Drained drain(media::AudioDecoder& decoder) {
    Drained out;
    out.rate = decoder.outputSampleRate();
    while (true) {
        auto buffer = decoder.nextBuffer();
        if (!buffer) {
            break;
        }
        if (out.peaks.empty()) {
            out.peaks.assign(static_cast<std::size_t>(buffer->channelCount()), 0.0F);
        }
        for (std::int32_t c = 0; c < buffer->channelCount(); ++c) {
            out.peaks[static_cast<std::size_t>(c)] =
                std::max(out.peaks[static_cast<std::size_t>(c)], buffer->peak(c));
        }
        out.samples += buffer->sampleCount();
    }
    return out;
}

}  // namespace

TEST_CASE("Audio decodes to canonical planar float", "[media][audio]") {
    ZARO_REQUIRE_FIXTURE("tone_48k.wav");

    auto opened = platform::ffmpeg::openAudioDecoder(fixture("tone_48k.wav"));
    REQUIRE(opened);
    media::AudioDecoder& decoder = **opened;

    CHECK(decoder.info().channelCount == 2);
    CHECK(decoder.outputSampleRate() == time::rates::hz48000);

    const Drained result = drain(decoder);
    REQUIRE(result.peaks.size() == 2);

    // Five seconds at 48kHz. Codec priming can shift this by a frame or two.
    CHECK(result.samples >= 239000);
    CHECK(result.samples <= 241000);

    // Both channels carry a full-scale sine, so both peak near 1.0. A silent
    // channel here would mean deinterleaving went wrong.
    CHECK(result.peaks[0] > 0.9F);
    CHECK(result.peaks[1] > 0.9F);
}

TEST_CASE("Audio resampling changes rate and sample count together", "[media][audio]") {
    ZARO_REQUIRE_FIXTURE("tone_48k.wav");

    auto opened =
        platform::ffmpeg::openAudioDecoder(fixture("tone_48k.wav"), {}, time::rates::hz44100);
    REQUIRE(opened);
    media::AudioDecoder& decoder = **opened;
    CHECK(decoder.outputSampleRate() == time::rates::hz44100);

    const Drained result = drain(decoder);
    // Five seconds at 44100 rather than 48000. The resampler's filter delay
    // accounts for the tolerance.
    CHECK(result.samples > 219000);
    CHECK(result.samples < 221500);
    CHECK(result.peaks[0] > 0.9F);
}

TEST_CASE("Audio timestamps advance monotonically", "[media][audio]") {
    ZARO_REQUIRE_FIXTURE("sync_click_flash.mov");

    auto opened = platform::ffmpeg::openAudioDecoder(fixture("sync_click_flash.mov"));
    REQUIRE(opened);
    media::AudioDecoder& decoder = **opened;

    time::RationalTime previous{-1, decoder.outputSampleRate()};
    std::int64_t buffers = 0;
    while (buffers < 50) {
        auto buffer = decoder.nextBuffer();
        if (!buffer) {
            break;
        }
        CHECK(buffer->pts() > previous);
        previous = buffer->pts();
        ++buffers;
    }
    CHECK(buffers > 0);
}

TEST_CASE("Audio seek lands near the requested time", "[media][audio]") {
    ZARO_REQUIRE_FIXTURE("sync_click_flash.mov");

    auto opened = platform::ffmpeg::openAudioDecoder(fixture("sync_click_flash.mov"));
    REQUIRE(opened);
    media::AudioDecoder& decoder = **opened;

    const time::RationalTime target =
        time::RationalTime::fromSeconds(time::Rational::fromInt(5), decoder.outputSampleRate());
    REQUIRE(decoder.seek(target).ok());

    auto buffer = decoder.nextBuffer();
    REQUIRE(buffer);
    const double landed = buffer->pts().toSecondsDouble();
    INFO("landed at " << landed << "s");
    CHECK(landed >= 4.5);
    CHECK(landed <= 5.5);
}

TEST_CASE("Opening audio on a file with none fails cleanly", "[media][audio]") {
    ZARO_REQUIRE_FIXTURE("ladder_prores.mov");
    const auto opened = platform::ffmpeg::openAudioDecoder(fixture("ladder_prores.mov"));
    REQUIRE_FALSE(opened);
    CHECK(opened.error().code() == ErrorCode::NotFound);
}
