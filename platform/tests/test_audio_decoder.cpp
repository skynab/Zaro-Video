#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "zaro/platform/ffmpeg/FFmpegMedia.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"

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

// A resampled read has to be continuous across block boundaries, whatever
// size the blocks are.
//
// This is the layer the "robotic playback" bug lived in, and the reason it
// survived a fix one level up: AudioGraph's mapping was made sample accurate,
// but ProjectMediaSource is what actually decodes and resamples, and every
// fixture the earlier tests used was already at the sequence's rate. A 44.1kHz
// file in a 48kHz sequence -- which is most music, and most of what a phone
// records -- takes a different path through here, and nothing covered it.
//
// The audio device asks in 1024-sample blocks; an export asks in whole video
// frames. Both must produce the same samples, because they are the same
// recording.
TEST_CASE("A resampled read is continuous whatever the block size",
          "[audio][resample][mediasource]") {
    ZARO_REQUIRE_FIXTURE("tone_48k.wav");

    // Deliberately not the file's own rate: asking for 44100 from a 48000
    // source is the resampling path, and it is the one the device takes
    // whenever the sequence and the footage disagree.
    const time::Rational asked = time::rates::hz44100;

    const auto gather = [&](std::int64_t blockSize) {
        model::Project project;
        auto probed = platform::ffmpeg::probe(fixture("tone_48k.wav"));
        REQUIRE(probed);
        model::MediaRef ref;
        ref.id = project.ids().next<model::MediaRefTag>();
        ref.path = fixture("tone_48k.wav");
        ref.name = "tone_48k.wav";
        ref.info = *probed;
        const auto mediaId = project.addMedia(ref);

        auto opened = platform::ffmpeg::ProjectMediaSource::open(project);
        REQUIRE(opened);

        std::vector<float> out;
        media::AudioBuffer buffer;
        for (std::int64_t at = 0; at < 40000; at += blockSize) {
            const time::RationalTime from{at, asked};
            REQUIRE((*opened)->read(mediaId, from, blockSize, asked, buffer).ok());
            for (std::int64_t i = 0; i < buffer.sampleCount(); ++i) {
                out.push_back(buffer.channel(0)[i]);
            }
        }
        return out;
    };

    const std::vector<float> frameAligned = gather(1920);
    const std::vector<float> deviceSized = gather(1024);

    const std::size_t common = std::min(frameAligned.size(), deviceSized.size());
    REQUIRE(common > 30000);

    // Sample for sample. A resampler that is re-primed, flushed or asked to
    // start over at a block boundary shows up here as a burst of difference at
    // that boundary and nowhere else -- which through a speaker is the buzz at
    // the block rate that this whole exercise started with.
    std::size_t differing = 0;
    double worst = 0.0;
    std::size_t worstAt = 0;
    for (std::size_t i = 0; i < common; ++i) {
        const double delta =
            std::fabs(static_cast<double>(frameAligned[i]) - static_cast<double>(deviceSized[i]));
        if (delta > 1e-4) {
            ++differing;
            if (delta > worst) {
                worst = delta;
                worstAt = i;
            }
        }
    }
    INFO("worst difference " << worst << " at sample " << worstAt << " of " << common << "; "
                             << differing << " samples differ");
    CHECK(differing == 0);
}
