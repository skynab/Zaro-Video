#include <algorithm>
#include <cmath>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/ColorPipeline.h"

using namespace zaro;
using Catch::Approx;
using media::ColorInfo;
using media::ColorMatrix;
using media::ColorRange;
using media::PixelFormat;
using media::TransferFunction;

namespace {

/// A flat 8-bit 4:2:0 frame with the given Y'CbCr values and tags.
media::VideoFrame flatFrame(std::uint8_t y, std::uint8_t cb, std::uint8_t cr,
                            const ColorInfo& color, std::int32_t size = 4) {
    media::VideoFrame frame = media::VideoFrame::allocate(size, size, PixelFormat::YUV420P);
    frame.setColor(color);
    for (std::int32_t row = 0; row < size; ++row) {
        std::fill_n(frame.plane(0) + row * frame.stride(0), size, y);
    }
    for (std::int32_t row = 0; row < size / 2; ++row) {
        std::fill_n(frame.plane(1) + row * frame.stride(1), size / 2, cb);
        std::fill_n(frame.plane(2) + row * frame.stride(2), size / 2, cr);
    }
    return frame;
}

ColorInfo rec709Limited() {
    return ColorInfo{media::ColorPrimaries::BT709, TransferFunction::BT709, ColorMatrix::BT709,
                     ColorRange::Limited};
}

}  // namespace

TEST_CASE("Transfer curves invert exactly", "[render][color]") {
    // The property users notice immediately when it breaks: a clip that passes
    // through the pipeline untouched must come out unchanged.
    const TransferFunction curves[] = {TransferFunction::BT709, TransferFunction::SRGB,
                                       TransferFunction::Gamma22, TransferFunction::Linear};
    for (const TransferFunction curve : curves) {
        for (int step = 0; step <= 100; ++step) {
            const float encoded = static_cast<float>(step) / 100.0F;
            const float linear = render::toLinearScalar(encoded, curve);
            const float back = render::fromLinearScalar(linear, curve);
            INFO("curve " << media::toString(curve) << " at " << encoded);
            CHECK(back == Approx(encoded).margin(1e-5));
        }
    }
}

TEST_CASE("Linear light is not gamma light", "[render][color]") {
    // The reason for compositing in linear at all: mid-grey in a gamma-encoded
    // signal carries about a fifth of the light of white, not half.
    const float midGrey = render::toLinearScalar(0.5F, TransferFunction::BT709);
    CHECK(midGrey > 0.15F);
    CHECK(midGrey < 0.30F);
    CHECK(render::toLinearScalar(0.0F, TransferFunction::BT709) == Approx(0.0F).margin(1e-6));
    CHECK(render::toLinearScalar(1.0F, TransferFunction::BT709) == Approx(1.0F).margin(1e-5));
}

TEST_CASE("Limited-range levels map to black and white", "[render][color]") {
    render::RgbaImage image;

    SECTION("code 16 is black") {
        const media::VideoFrame frame = flatFrame(16, 128, 128, rec709Limited());
        REQUIRE(render::toLinear(frame, image).ok());
        CHECK(image.at(0, 0).r == Approx(0.0F).margin(1e-4));
        CHECK(image.at(0, 0).a == 1.0F);
    }

    SECTION("code 235 is white") {
        const media::VideoFrame frame = flatFrame(235, 128, 128, rec709Limited());
        REQUIRE(render::toLinear(frame, image).ok());
        CHECK(image.at(0, 0).r == Approx(1.0F).margin(1e-4));
        CHECK(image.at(0, 0).g == Approx(1.0F).margin(1e-4));
        CHECK(image.at(0, 0).b == Approx(1.0F).margin(1e-4));
    }

    SECTION("grey stays neutral: R, G and B agree") {
        const media::VideoFrame frame = flatFrame(126, 128, 128, rec709Limited());
        REQUIRE(render::toLinear(frame, image).ok());
        const render::Rgba pixel = image.at(1, 1);
        CHECK(pixel.r == Approx(pixel.g).margin(1e-5));
        CHECK(pixel.g == Approx(pixel.b).margin(1e-5));
    }
}

TEST_CASE("Full range is not read as limited", "[render][color]") {
    // Treating full range as limited crushes blacks and clips highlights; it is
    // the most common colour bug in video and it has to be impossible here.
    ColorInfo full = rec709Limited();
    full.range = ColorRange::Full;

    render::RgbaImage limitedImage;
    render::RgbaImage fullImage;
    REQUIRE(render::toLinear(flatFrame(16, 128, 128, rec709Limited()), limitedImage).ok());
    REQUIRE(render::toLinear(flatFrame(16, 128, 128, full), fullImage).ok());

    // Code 16 is black under limited range and a dark grey under full.
    CHECK(limitedImage.at(0, 0).r == Approx(0.0F).margin(1e-4));
    CHECK(fullImage.at(0, 0).r > 0.0F);

    render::RgbaImage fullWhite;
    REQUIRE(render::toLinear(flatFrame(255, 128, 128, full), fullWhite).ok());
    CHECK(fullWhite.at(0, 0).r == Approx(1.0F).margin(1e-4));
}

TEST_CASE("The luma matrix changes the colour, as it should", "[render][color]") {
    // Same numbers, different matrix tag: the result must differ, or the tag is
    // being ignored and every SD clip will be subtly tinted.
    ColorInfo bt601 = rec709Limited();
    bt601.matrix = ColorMatrix::BT601;

    render::RgbaImage as709;
    render::RgbaImage as601;
    REQUIRE(render::toLinear(flatFrame(120, 90, 160, rec709Limited()), as709).ok());
    REQUIRE(render::toLinear(flatFrame(120, 90, 160, bt601), as601).ok());

    CHECK(as709.at(0, 0).g != Approx(as601.at(0, 0).g).margin(1e-4));

    SECTION("but neutral grey is neutral under either") {
        render::RgbaImage grey709;
        render::RgbaImage grey601;
        REQUIRE(render::toLinear(flatFrame(150, 128, 128, rec709Limited()), grey709).ok());
        REQUIRE(render::toLinear(flatFrame(150, 128, 128, bt601), grey601).ok());
        CHECK(grey709.at(0, 0).r == Approx(grey601.at(0, 0).r).margin(1e-5));
    }
}

TEST_CASE("A frame round trips through the working space", "[render][color]") {
    // Decode to linear, encode back to 8-bit: the values that survive an 8-bit
    // quantisation must come back where they started.
    for (const int code : {16, 40, 80, 126, 180, 235}) {
        const media::VideoFrame frame =
            flatFrame(static_cast<std::uint8_t>(code), 128, 128, rec709Limited());
        render::RgbaImage image;
        REQUIRE(render::toLinear(frame, image).ok());

        std::vector<std::uint8_t> rgb(static_cast<std::size_t>(image.width()) * 3 *
                                      static_cast<std::size_t>(image.height()));
        REQUIRE(render::toDisplayRgb24(image, rgb.data(), image.width() * 3).ok());

        // Limited 16-235 expands to full 0-255 on the way out.
        const int expected = static_cast<int>(std::lround((code - 16) / 219.0 * 255.0));
        INFO("code " << code);
        CHECK(std::abs(static_cast<int>(rgb[0]) - expected) <= 1);
    }
}

TEST_CASE("HDR is refused rather than mistreated", "[render][color]") {
    // Treating PQ as Rec.709 does not look subtly wrong, it looks broken. A
    // clear error beats a picture nobody can explain.
    ColorInfo pq = rec709Limited();
    pq.transfer = TransferFunction::PQ;
    CHECK_FALSE(render::isSupported(pq));

    render::RgbaImage image;
    const auto status = render::toLinear(flatFrame(128, 128, 128, pq), image);
    REQUIRE_FALSE(status.ok());
    CHECK(status.error().code() == ErrorCode::Unsupported);
    CHECK(status.error().message().find("pq") != std::string::npos);
}

TEST_CASE("10-bit sources decode to the same values as 8-bit", "[render][color]") {
    media::VideoFrame deep = media::VideoFrame::allocate(4, 4, PixelFormat::YUV422P10);
    deep.setColor(rec709Limited());
    const auto write = [&deep](std::size_t plane, std::uint16_t value, std::int32_t width) {
        for (std::int32_t row = 0; row < 4; ++row) {
            auto* pixels =
                reinterpret_cast<std::uint16_t*>(deep.plane(plane) + row * deep.stride(plane));
            std::fill_n(pixels, width, value);
        }
    };
    write(0, 235 << 2, 4);  // white at 10-bit
    write(1, 128 << 2, 2);
    write(2, 128 << 2, 2);

    render::RgbaImage image;
    REQUIRE(render::toLinear(deep, image).ok());
    CHECK(image.at(0, 0).r == Approx(1.0F).margin(1e-3));
}
