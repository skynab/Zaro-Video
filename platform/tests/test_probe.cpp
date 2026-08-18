#include <catch2/catch_test_macros.hpp>

#include "zaro/platform/ffmpeg/FFmpegMedia.h"

#include "Fixtures.h"

using namespace zaro;
using zaro::testing::fixture;

TEST_CASE("Probe reads basic structure", "[media][probe]") {
    ZARO_REQUIRE_FIXTURE("ladder_prores.mov");

    const auto probed = platform::ffmpeg::probe(fixture("ladder_prores.mov"));
    REQUIRE(probed);
    const media::VideoStreamInfo* video = probed->primaryVideo();
    REQUIRE(video != nullptr);

    CHECK(video->codecName == "prores");
    CHECK(video->width == 320);
    CHECK(video->height == 240);
    CHECK(video->frameRate == time::rates::fps25);
    CHECK(video->pixelFormat == media::PixelFormat::YUV422P10);
    CHECK_FALSE(video->isVariableFrameRate);
}

TEST_CASE("Probe reads drop-frame start timecode", "[media][probe][timecode]") {
    ZARO_REQUIRE_FIXTURE("pattern_2997df.mov");

    const auto probed = platform::ffmpeg::probe(fixture("pattern_2997df.mov"));
    REQUIRE(probed);
    const media::VideoStreamInfo* video = probed->primaryVideo();
    REQUIRE(video != nullptr);

    CHECK(video->frameRate == time::rates::fps29_97);

    REQUIRE(video->startTimecode.has_value());
    CHECK(video->startTimecode->dropFrame);
    CHECK(video->startTimecode->hours == 1);
    CHECK(video->startTimecode->toString() == "01:00:00;00");

    // And it converts to the frame index our own timecode code says it should.
    const auto asFrames = time::framesFromTimecode(*video->startTimecode, video->frameRate);
    REQUIRE(asFrames.has_value());
    CHECK(*asFrames == 107892);
}

TEST_CASE("Probe finds audio alongside video", "[media][probe]") {
    ZARO_REQUIRE_FIXTURE("sync_click_flash.mov");

    const auto probed = platform::ffmpeg::probe(fixture("sync_click_flash.mov"));
    REQUIRE(probed);
    REQUIRE(probed->primaryVideo() != nullptr);

    const media::AudioStreamInfo* audio = probed->primaryAudio();
    REQUIRE(audio != nullptr);
    CHECK(audio->sampleRate == time::rates::hz48000);
    CHECK(audio->channelCount == 1);
    CHECK(audio->duration.toDouble() > 9.0);
}

TEST_CASE("Probe flags variable frame rate footage", "[media][probe][vfr]") {
    ZARO_REQUIRE_FIXTURE("vfr_sample.mp4");

    const auto probed = platform::ffmpeg::probe(fixture("vfr_sample.mp4"));
    REQUIRE(probed);
    const media::VideoStreamInfo* video = probed->primaryVideo();
    REQUIRE(video != nullptr);

    // The container's nominal rate and the observed average disagree, which is
    // the only hint available before conforming.
    INFO("nominal " << video->frameRate.toString() << ", average "
                    << video->averageFrameRate.toString());
    CHECK(video->isVariableFrameRate);
}

TEST_CASE("Probe tags colour, marking what it had to infer", "[media][probe][color]") {
    ZARO_REQUIRE_FIXTURE("ladder_h264.mp4");

    const auto probed = platform::ffmpeg::probe(fixture("ladder_h264.mp4"));
    REQUIRE(probed);
    const media::VideoStreamInfo* video = probed->primaryVideo();
    REQUIRE(video != nullptr);

    // Whatever the file did or did not declare, what comes out is complete.
    CHECK(video->color.isFullyTagged());
    // 320x240 is SD-shaped, so an untagged file resolves to BT.601.
    if (video->colorWasGuessed) {
        CHECK(video->color.matrix == media::ColorMatrix::BT601);
        CHECK(video->color.range == media::ColorRange::Limited);
    }
}

TEST_CASE("Probe reports audio-only files without a video stream", "[media][probe]") {
    ZARO_REQUIRE_FIXTURE("tone_48k.wav");

    const auto probed = platform::ffmpeg::probe(fixture("tone_48k.wav"));
    REQUIRE(probed);
    CHECK(probed->videoStreams.empty());
    REQUIRE(probed->primaryAudio() != nullptr);
    CHECK(probed->primaryAudio()->channelCount == 2);
    CHECK(probed->primaryAudio()->sampleRate == time::rates::hz48000);
}

TEST_CASE("Probe fails cleanly on nonsense", "[media][probe]") {
    SECTION("a file that does not exist") {
        const auto probed = platform::ffmpeg::probe("/nonexistent/path/to/nothing.mov");
        REQUIRE_FALSE(probed);
        CHECK(probed.error().code() == ErrorCode::NotFound);
    }

    SECTION("a file that is not media") {
        const auto probed = platform::ffmpeg::probe(__FILE__);
        REQUIRE_FALSE(probed);
    }
}
