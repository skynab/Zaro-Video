#include <catch2/catch_test_macros.hpp>

#include "zaro/core/media/AudioBuffer.h"
#include "zaro/core/media/ColorInfo.h"
#include "zaro/core/media/PixelFormat.h"
#include "zaro/core/media/VideoFrame.h"

using namespace zaro::media;
namespace rates = zaro::time::rates;

TEST_CASE("Pixel format geometry", "[media][pixelformat]") {
    SECTION("planar 4:2:0 halves chroma in both directions") {
        CHECK(rowBytes(PixelFormat::YUV420P, 1920, 0) == 1920);
        CHECK(rowBytes(PixelFormat::YUV420P, 1920, 1) == 960);
        CHECK(planeHeight(PixelFormat::YUV420P, 1080, 0) == 1080);
        CHECK(planeHeight(PixelFormat::YUV420P, 1080, 1) == 540);
    }

    SECTION("4:2:2 halves horizontally only") {
        CHECK(rowBytes(PixelFormat::YUV422P, 1920, 1) == 960);
        CHECK(planeHeight(PixelFormat::YUV422P, 1080, 1) == 1080);
    }

    SECTION("10-bit planes are two bytes per sample") {
        CHECK(rowBytes(PixelFormat::YUV422P10, 1920, 0) == 3840);
        CHECK(rowBytes(PixelFormat::YUV422P10, 1920, 1) == 1920);
    }

    SECTION("NV12 interleaves chroma, so its second plane is full width") {
        CHECK(info(PixelFormat::NV12).planeCount == 2);
        CHECK(rowBytes(PixelFormat::NV12, 1920, 1) == 1920);
        CHECK(planeHeight(PixelFormat::NV12, 1080, 1) == 540);
    }

    SECTION("odd dimensions round chroma up rather than truncating") {
        CHECK(rowBytes(PixelFormat::YUV420P, 1921, 1) == 961);
        CHECK(planeHeight(PixelFormat::YUV420P, 1081, 1) == 541);
    }

    SECTION("packed RGB") {
        CHECK(rowBytes(PixelFormat::RGB24, 100, 0) == 300);
        CHECK(rowBytes(PixelFormat::RGBA8, 100, 0) == 400);
        CHECK(info(PixelFormat::RGBA8).hasAlpha);
    }
}

TEST_CASE("VideoFrame allocation and layout", "[media][videoframe]") {
    VideoFrame frame = VideoFrame::allocate(1920, 1080, PixelFormat::YUV420P);
    REQUIRE(frame.isValid());
    CHECK(frame.width() == 1920);
    CHECK(frame.height() == 1080);
    CHECK(frame.planeCount() == 3);

    SECTION("strides are aligned and at least as wide as the row") {
        for (std::size_t plane = 0; plane < frame.planeCount(); ++plane) {
            const auto index = static_cast<std::int32_t>(plane);
            CHECK(frame.stride(plane) >= rowBytes(PixelFormat::YUV420P, 1920, index));
            CHECK(frame.stride(plane) % 64 == 0);
        }
    }

    SECTION("planes do not overlap") {
        CHECK(frame.plane(1) > frame.plane(0));
        CHECK(frame.plane(2) > frame.plane(1));
        const auto lumaBytes = static_cast<std::size_t>(frame.stride(0)) * 1080;
        CHECK(static_cast<std::size_t>(frame.plane(1) - frame.plane(0)) >= lumaBytes);
    }

    SECTION("odd dimensions still allocate a usable frame") {
        VideoFrame odd = VideoFrame::allocate(1921, 1081, PixelFormat::YUV420P);
        REQUIRE(odd.isValid());
        CHECK(odd.stride(1) >= 961);
    }
}

TEST_CASE("VideoFrame clone copies pixels and metadata", "[media][videoframe]") {
    VideoFrame frame = VideoFrame::allocate(64, 64, PixelFormat::YUV420P);
    frame.setPts(zaro::time::RationalTime{42, rates::fps25});
    frame.setKeyframe(true);
    frame.setSourceIndex(42);
    frame.setColor(ColorInfo{ColorPrimaries::BT709, TransferFunction::BT709, ColorMatrix::BT709,
                             ColorRange::Limited});
    for (std::int32_t y = 0; y < 64; ++y) {
        for (std::int32_t x = 0; x < 64; ++x) {
            frame.plane(0)[static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.stride(0)) +
                           static_cast<std::size_t>(x)] = static_cast<std::uint8_t>((x + y) & 0xFF);
        }
    }

    const VideoFrame copy = frame.clone();
    REQUIRE(copy.isValid());
    CHECK(copy.pts() == frame.pts());
    CHECK(copy.isKeyframe());
    CHECK(copy.sourceIndex() == 42);
    CHECK(copy.color() == frame.color());
    CHECK(copy.sampleAt(10, 20) == frame.sampleAt(10, 20));
    CHECK(copy.sampleAt(63, 63) == frame.sampleAt(63, 63));
    // Distinct storage, not an aliased view.
    CHECK(copy.plane(0) != frame.plane(0));
}

TEST_CASE("Untagged colour resolves by the industry convention", "[media][color]") {
    const ColorInfo untagged;

    SECTION("HD gets BT.709") {
        const ColorInfo hd = untagged.resolved(1920, 1080);
        CHECK(hd.primaries == ColorPrimaries::BT709);
        CHECK(hd.matrix == ColorMatrix::BT709);
        CHECK(hd.range == ColorRange::Limited);
    }

    SECTION("NTSC-shaped SD gets 601 525-line") {
        const ColorInfo sd = untagged.resolved(720, 480);
        CHECK(sd.primaries == ColorPrimaries::BT601_525);
        CHECK(sd.matrix == ColorMatrix::BT601);
    }

    SECTION("PAL-shaped SD gets 601 625-line") {
        const ColorInfo sd = untagged.resolved(720, 576);
        CHECK(sd.primaries == ColorPrimaries::BT601_625);
    }

    SECTION("UHD is above the SD threshold, so 709 not 601") {
        CHECK(untagged.resolved(3840, 2160).matrix == ColorMatrix::BT709);
    }

    SECTION("tags that are present are never overwritten") {
        ColorInfo tagged;
        tagged.matrix = ColorMatrix::BT2020NCL;
        tagged.range = ColorRange::Full;
        const ColorInfo out = tagged.resolved(720, 480);
        CHECK(out.matrix == ColorMatrix::BT2020NCL);
        CHECK(out.range == ColorRange::Full);
        CHECK(out.primaries == ColorPrimaries::BT601_525);  // still filled in
    }

    SECTION("isFullyTagged distinguishes read from inferred") {
        CHECK_FALSE(untagged.isFullyTagged());
        CHECK(untagged.resolved(1920, 1080).isFullyTagged());
    }
}

TEST_CASE("AudioBuffer basics", "[media][audio]") {
    AudioBuffer buffer{2, 480, rates::hz48000};
    REQUIRE(buffer.isValid());
    CHECK(buffer.channelCount() == 2);
    CHECK(buffer.sampleCount() == 480);
    CHECK(buffer.duration().toSeconds() == zaro::time::Rational{1, 100});

    buffer.channel(0)[0] = 0.5F;
    buffer.channel(1)[10] = -0.75F;
    CHECK(buffer.peak(0) == 0.5F);
    CHECK(buffer.peak(1) == 0.75F);

    SECTION("resize preserves the existing samples and zero-fills growth") {
        buffer.resize(960);
        CHECK(buffer.sampleCount() == 960);
        CHECK(buffer.channel(0)[0] == 0.5F);
        CHECK(buffer.channel(0)[959] == 0.0F);
    }
}
