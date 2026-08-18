#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "zaro/platform/ffmpeg/FFmpegMedia.h"

#include "Fixtures.h"

using namespace zaro;
using zaro::testing::expectedLadderLuma;
using zaro::testing::fixture;

namespace {

/// Read the ladder value out of a decoded frame, normalising whatever depth and
/// layout the decoder happened to produce back to the 8-bit number that
/// testdata/generate.sh wrote in.
///
/// Software decode gives planar 8- or 10-bit; VideoToolbox gives NV12 or P010.
/// All four carry luma in plane 0, which is the whole reason this fixture works
/// as a cross-path identity check.
int ladderLuma(const media::VideoFrame& frame) {
    const std::uint16_t raw = frame.sampleAt(frame.width() / 2, frame.height() / 2, 0);
    switch (frame.format()) {
        case media::PixelFormat::YUV420P10:
        case media::PixelFormat::YUV422P10:
        case media::PixelFormat::YUV444P10:
            return raw >> 2;
        case media::PixelFormat::P010:
            // P010 left-justifies 10 bits in a 16-bit word.
            return raw >> 8;
        default:
            return raw;
    }
}

std::unique_ptr<media::VideoDecoder> open(const std::string& name, media::DecodeMode mode) {
    media::DecoderOptions options;
    options.mode = mode;
    auto opened = platform::ffmpeg::openVideoDecoder(fixture(name), options);
    if (!opened) {
        INFO(opened.error().toString());
        return nullptr;
    }
    return std::move(*opened);
}

}  // namespace

TEST_CASE("Sequential decode walks the ladder in order", "[media][decode]") {
    ZARO_REQUIRE_FIXTURE("ladder_prores.mov");

    auto decoder = open("ladder_prores.mov", media::DecodeMode::ForceSoftware);
    REQUIRE(decoder != nullptr);

    for (std::int64_t index = 0; index < 300; ++index) {
        auto frame = decoder->nextFrame();
        REQUIRE(frame);
        INFO("frame " << index);
        // ProRes is visually lossless on a flat field, so this is exact.
        CHECK(ladderLuma(*frame) == expectedLadderLuma(index));
    }
    CHECK_FALSE(decoder->nextFrame());
}

TEST_CASE("Random access is frame exact", "[media][decode][seek]") {
    ZARO_REQUIRE_FIXTURE("ladder_prores.mov");

    auto decoder = open("ladder_prores.mov", media::DecodeMode::ForceSoftware);
    REQUIRE(decoder != nullptr);

    // Deliberately awkward: long backward jumps, short forward hops, repeats,
    // and both ends of the file. Off-by-one seeks survive tidy access patterns.
    const std::vector<std::int64_t> order{0,  299, 1,   150, 149, 151, 42, 299, 298, 0,   75,
                                          76, 77,  200, 199, 12,  287, 3,  3,   250, 100, 101};
    for (const std::int64_t index : order) {
        auto frame = decoder->frameAtIndex(index);
        REQUIRE(frame);
        INFO("frame " << index);
        CHECK(ladderLuma(*frame) == expectedLadderLuma(index));
        CHECK(frame->sourceIndex() == index);
    }
}

TEST_CASE("Every frame is reachable by index, in every codec", "[media][decode][seek]") {
    // Each index is requested cold, in order, but through frameAtIndex rather
    // than sequential decode -- so this exercises 300 separate resolutions of
    // index to timestamp to picture. Long-GOP codecs with B-frames are where
    // that goes wrong, so all three ladder encodings are covered, not just the
    // all-intra one where seeking is trivially correct.
    struct Case {
        const char* file;
        int tolerance;
    };
    const Case cases[] = {
        {"ladder_prores.mov", 0},  // all-intra, visually lossless: exact
        {"ladder_h264.mp4", 2},
        {"ladder_hevc.mp4", 2},
    };

    for (const Case& c : cases) {
        ZARO_REQUIRE_FIXTURE(c.file);
        auto decoder = open(c.file, media::DecodeMode::ForceSoftware);
        REQUIRE(decoder != nullptr);

        const auto count = decoder->frameCount();
        REQUIRE(count);
        CHECK(*count == 300);

        for (std::int64_t index = 0; index < *count; ++index) {
            auto frame = decoder->frameAtIndex(index);
            REQUIRE(frame);
            const int actual = ladderLuma(*frame);
            const int expected = expectedLadderLuma(index);
            if (std::abs(actual - expected) > c.tolerance) {
                FAIL(c.file << " frame " << index << " decoded luma " << actual << ", expected "
                            << expected);
            }
        }
    }
    SUCCEED("900 frames individually addressed across three codecs");
}

TEST_CASE("Backward random access is exact in long-GOP codecs", "[media][decode][seek]") {
    // Walking backwards forces a fresh seek for every single frame, which is
    // the access pattern that exposed the DTS-versus-PTS keyframe bug: with
    // B-frames the keyframe indexed before a target can present after it, and
    // decoding forward from there never reaches the frame at all.
    for (const char* name : {"ladder_hevc.mp4", "ladder_h264.mp4"}) {
        ZARO_REQUIRE_FIXTURE(name);
        auto decoder = open(name, media::DecodeMode::ForceSoftware);
        REQUIRE(decoder != nullptr);

        for (std::int64_t index = 60; index >= 0; --index) {
            auto frame = decoder->frameAtIndex(index);
            REQUIRE(frame);
            const int actual = ladderLuma(*frame);
            const int expected = expectedLadderLuma(index);
            if (std::abs(actual - expected) > 2) {
                FAIL(name << " frame " << index << " decoded luma " << actual << ", expected "
                          << expected);
            }
        }
    }
    SUCCEED("122 frames resolved in reverse order");
}

TEST_CASE("Lossy codecs are frame exact in identity, if not in value", "[media][decode][seek]") {
    // The point here is not that H.264 reproduces the exact luma -- it does not
    // -- but that seeking lands on the right frame. Ladder steps are four code
    // values apart and compression noise runs to one or two, so a tolerance of
    // two catches a one-frame error while still absorbing the codec.
    //
    // HEVC frame 12 is the case that matters: it is the last frame before a
    // keyframe whose DTS precedes it but whose PTS follows it, which is exactly
    // where a seek lands too late.
    for (const char* name : {"ladder_h264.mp4", "ladder_hevc.mp4"}) {
        ZARO_REQUIRE_FIXTURE(name);
        auto decoder = open(name, media::DecodeMode::ForceSoftware);
        REQUIRE(decoder != nullptr);

        for (const std::int64_t index : {0, 47, 46, 200, 11, 12, 13, 299, 133, 134}) {
            auto frame = decoder->frameAtIndex(index);
            REQUIRE(frame);
            const int actual = ladderLuma(*frame);
            const int expected = expectedLadderLuma(index);
            INFO(name << " frame " << index << ": got " << actual << ", expected " << expected);
            CHECK(std::abs(actual - expected) <= 2);
        }
    }
}

TEST_CASE("Hardware and software decode agree", "[media][decode][hwaccel]") {
    ZARO_REQUIRE_FIXTURE("ladder_h264.mp4");

    media::DecoderOptions hardwareOptions;
    hardwareOptions.mode = media::DecodeMode::ForceHardware;
    auto opened = platform::ffmpeg::openVideoDecoder(fixture("ladder_h264.mp4"), hardwareOptions);
    if (!opened) {
        SKIP("no hardware decoder available on this machine");
    }
    std::unique_ptr<media::VideoDecoder> hardware = std::move(*opened);
    if (!hardware->usingHardware()) {
        SKIP("hardware decoder declined this stream");
    }
    auto software = open("ladder_h264.mp4", media::DecodeMode::ForceSoftware);
    REQUIRE(software != nullptr);
    CHECK_FALSE(software->usingHardware());

    for (const std::int64_t index : {0, 33, 120, 121, 250, 299}) {
        auto hardwareFrame = hardware->frameAtIndex(index);
        auto softwareFrame = software->frameAtIndex(index);
        REQUIRE(hardwareFrame);
        REQUIRE(softwareFrame);

        INFO("frame " << index << ": hardware format " << media::toString(hardwareFrame->format())
                      << ", software format " << media::toString(softwareFrame->format()));
        // The pixel layouts differ -- NV12 versus planar -- but they must be
        // showing the same picture.
        CHECK(std::abs(ladderLuma(*hardwareFrame) - ladderLuma(*softwareFrame)) <= 1);
        CHECK(hardwareFrame->pts() == softwareFrame->pts());
        CHECK(hardwareFrame->width() == softwareFrame->width());
    }
}

TEST_CASE("Variable frame rate footage is addressable by index", "[media][decode][vfr]") {
    ZARO_REQUIRE_FIXTURE("vfr_sample.mp4");

    auto decoder = open("vfr_sample.mp4", media::DecodeMode::ForceSoftware);
    REQUIRE(decoder != nullptr);
    REQUIRE(decoder->info().isVariableFrameRate);

    const auto count = decoder->frameCount();
    REQUIRE(count);
    CHECK(*count > 100);

    // Timestamps are unevenly spaced, so index-times-duration arithmetic cannot
    // work here. Conforming is the only route to an exact answer, and every
    // index must still resolve to a distinct, ordered frame.
    time::RationalTime previous{-1, decoder->info().frameRate};
    for (std::int64_t index = 0; index < *count; index += 7) {
        auto frame = decoder->frameAtIndex(index);
        REQUIRE(frame);
        INFO("vfr frame " << index << " at pts " << frame->pts().toString());
        CHECK(frame->pts() > previous);
        previous = frame->pts();
    }
}

TEST_CASE("frameAtTime returns the frame the playhead is inside", "[media][decode][seek]") {
    ZARO_REQUIRE_FIXTURE("ladder_prores.mov");

    auto decoder = open("ladder_prores.mov", media::DecodeMode::ForceSoftware);
    REQUIRE(decoder != nullptr);
    const time::Rational& rate = decoder->info().frameRate;

    SECTION("exactly on a frame boundary gives that frame") {
        auto frame = decoder->frameAtTime(time::RationalTime{100, rate});
        REQUIRE(frame);
        CHECK(ladderLuma(*frame) == expectedLadderLuma(100));
    }

    SECTION("part way through a frame still gives that frame, not the next") {
        // Half a frame past frame 100, expressed at a much finer rate.
        const time::RationalTime midway{201, time::Rational{50, 1}};
        auto frame = decoder->frameAtTime(midway);
        REQUIRE(frame);
        CHECK(ladderLuma(*frame) == expectedLadderLuma(100));
    }

    SECTION("before the start clamps to the first frame") {
        auto frame = decoder->frameAtTime(time::RationalTime{-10, rate});
        REQUIRE(frame);
        CHECK(ladderLuma(*frame) == expectedLadderLuma(0));
    }
}

TEST_CASE("Out of range access fails rather than guessing", "[media][decode]") {
    ZARO_REQUIRE_FIXTURE("ladder_prores.mov");

    auto decoder = open("ladder_prores.mov", media::DecodeMode::ForceSoftware);
    REQUIRE(decoder != nullptr);

    const auto past = decoder->frameAtIndex(10000);
    REQUIRE_FALSE(past);
    CHECK(past.error().code() == ErrorCode::NotFound);

    const auto negative = decoder->frameAtIndex(-1);
    REQUIRE_FALSE(negative);
}

TEST_CASE("Conforming reports the true frame count", "[media][decode][conform]") {
    ZARO_REQUIRE_FIXTURE("ladder_h264.mp4");

    auto decoder = open("ladder_h264.mp4", media::DecodeMode::ForceSoftware);
    REQUIRE(decoder != nullptr);
    CHECK_FALSE(decoder->isConformed());

    REQUIRE(decoder->conform().ok());
    CHECK(decoder->isConformed());

    const auto count = decoder->frameCount();
    REQUIRE(count);
    CHECK(*count == 300);

    SECTION("conforming twice is a no-op, and decoding still works after it") {
        REQUIRE(decoder->conform().ok());
        auto frame = decoder->frameAtIndex(42);
        REQUIRE(frame);
        CHECK(std::abs(ladderLuma(*frame) - expectedLadderLuma(42)) <= 2);
    }
}

TEST_CASE("Decoded frames carry resolved colour tags", "[media][decode][color]") {
    ZARO_REQUIRE_FIXTURE("ladder_prores.mov");

    auto decoder = open("ladder_prores.mov", media::DecodeMode::ForceSoftware);
    REQUIRE(decoder != nullptr);

    auto frame = decoder->nextFrame();
    REQUIRE(frame);
    // Never Unknown by the time a frame leaves the decoder -- that is the whole
    // contract, so that nothing downstream has to guess.
    CHECK(frame->color().isFullyTagged());
}

TEST_CASE("Auto mode picks software while frames are consumed on the CPU",
          "[media][decode][hwaccel]") {
    ZARO_REQUIRE_FIXTURE("ladder_h264.mp4");
    auto decoder = open("ladder_h264.mp4", media::DecodeMode::Auto);
    REQUIRE(decoder != nullptr);
    // Not a statement about what is available, but about what is faster. See
    // DecodeMode::Auto. When Phase 3 lands a GPU-side consumer, this
    // expectation is meant to be revisited, not worked around.
    CHECK_FALSE(decoder->usingHardware());
}

TEST_CASE("Opening video on an audio-only file fails cleanly", "[media][decode]") {
    ZARO_REQUIRE_FIXTURE("tone_48k.wav");
    const auto opened = platform::ffmpeg::openVideoDecoder(fixture("tone_48k.wav"));
    REQUIRE_FALSE(opened);
    CHECK(opened.error().code() == ErrorCode::NotFound);
}

TEST_CASE("Decode throughput", "[.benchmark][media][decode]") {
    // Hidden by default -- run explicitly with `zaro_media_tests [.benchmark]`
    // and generate its fixture with `testdata/generate.sh --with-perf`.
    //
    // This is the shape of the performance gate the plan calls for in CI. It
    // asserts a floor rather than a target, because the number that matters is
    // whether realtime playback is achievable, not how far above it we are.
    ZARO_REQUIRE_FIXTURE("perf_4k_prores.mov");

    auto decoder = open("perf_4k_prores.mov", media::DecodeMode::Auto);
    REQUIRE(decoder != nullptr);

    const auto start = std::chrono::steady_clock::now();
    std::int64_t decoded = 0;
    while (decoded < 150) {
        auto frame = decoder->nextFrame();
        if (!frame) {
            break;
        }
        ++decoded;
    }
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double fps = static_cast<double>(decoded) / elapsed;
    const double realtime = fps / decoder->info().frameRate.toDouble();

    WARN("4K ProRes: " << fps << " fps, " << realtime << "x realtime ("
                       << (decoder->usingHardware() ? "hardware" : "software") << ")");
    CHECK(decoded == 150);
    CHECK(realtime > 1.0);
}
