#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/ColorPipeline.h"
#include "zaro/core/render/ToneMap.h"

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

TEST_CASE("A curve with no formula is refused rather than guessed at", "[render][color]") {
    // Treating an unknown curve as Rec.709 does not look subtly wrong, it looks
    // broken -- and worse, it looks broken in a way nobody can see is a tag
    // rather than the footage. A clear error beats a picture nobody can
    // explain. PQ and HLG used to be here; they have formulas now.
    ColorInfo unknown = rec709Limited();
    unknown.transfer = TransferFunction::Unknown;
    CHECK_FALSE(render::isSupported(unknown));

    render::RgbaImage image;
    const auto status = render::toLinear(flatFrame(128, 128, 128, unknown), image);
    REQUIRE_FALSE(status.ok());
    CHECK(status.error().code() == ErrorCode::Unsupported);

    SECTION("and the HDR curves are handled now") {
        for (const TransferFunction transfer : {TransferFunction::PQ, TransferFunction::HLG}) {
            ColorInfo hdr = rec709Limited();
            hdr.transfer = transfer;
            INFO(media::toString(transfer));
            CHECK(render::isSupported(hdr));
            render::RgbaImage decoded;
            CHECK(render::toLinear(flatFrame(128, 128, 128, hdr), decoded).ok());
        }
    }
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

TEST_CASE("The camera log curves hit their published anchors", "[render][color][log]") {
    // Every one of these is a number from the manufacturer's own document, and
    // reading them back against it is the only useful check there is: a log
    // curve that is slightly wrong produces a picture that looks plausible and
    // grades badly, which is the worst way for this to fail.
    struct Anchor {
        const char* name;
        TransferFunction transfer;
        float scene;    // reflectance
        float encoded;  // what the camera writes for it
        float tolerance;
    };
    const Anchor anchors[] = {
        // Sony: 18% grey sits at code 420 of 1023.
        {"S-Log3 mid grey", TransferFunction::SLog3, 0.18F, 420.0F / 1023.0F, 0.001F},
        // And 90% white at code 598.
        {"S-Log3 white", TransferFunction::SLog3, 0.90F, 598.0F / 1023.0F, 0.002F},
        // Panasonic: 18% grey at 42.3 IRE.
        {"V-Log mid grey", TransferFunction::VLog, 0.18F, 0.423F, 0.002F},
        // Arri: 18% grey at 39.1% of the code range for EI 800.
        {"LogC3 mid grey", TransferFunction::LogC3, 0.18F, 0.391F, 0.002F},
    };

    for (const Anchor& anchor : anchors) {
        INFO(anchor.name);
        CHECK(render::fromLinearScalar(anchor.scene, anchor.transfer) ==
              Approx(anchor.encoded).margin(anchor.tolerance));
        CHECK(render::toLinearScalar(anchor.encoded, anchor.transfer) ==
              Approx(anchor.scene).margin(0.01F));
    }
}

TEST_CASE("Every transfer curve round trips", "[render][color][log]") {
    // The inverse is what an export uses and the forward is what a decode uses;
    // if they disagree, a file written and read back does not come out the same
    // and nothing in between says so.
    const TransferFunction all[] = {
        TransferFunction::Linear,  TransferFunction::SRGB,  TransferFunction::Gamma22,
        TransferFunction::Gamma28, TransferFunction::BT709, TransferFunction::SMPTE170M,
        TransferFunction::PQ,      TransferFunction::HLG,   TransferFunction::SLog3,
        TransferFunction::VLog,    TransferFunction::LogC3,
    };
    for (const TransferFunction transfer : all) {
        for (const float encoded : {0.0F, 0.05F, 0.2F, 0.5F, 0.75F, 1.0F}) {
            INFO(media::toString(transfer) << " at " << encoded);
            const float linear = render::toLinearScalar(encoded, transfer);
            CHECK(render::fromLinearScalar(linear, transfer) == Approx(encoded).margin(0.002F));
        }
    }
}

TEST_CASE("PQ arrives with diffuse white near one", "[render][color][log]") {
    // PQ carries absolute light and the working space carries relative light,
    // so the two need a reference to agree on. 100 cd/m2 -- SDR diffuse white --
    // is the one chosen, which puts graphic white at 1.0 and leaves specular
    // highlights above it, exactly what a scene-linear space is for. Any other
    // choice makes a correctly exposed HDR shot arrive a hundred times too dark
    // or too bright.
    const float codeFor100Nits = render::fromLinearScalar(1.0F, TransferFunction::PQ);
    CHECK(codeFor100Nits > 0.4F);
    CHECK(codeFor100Nits < 0.65F);
    CHECK(render::toLinearScalar(codeFor100Nits, TransferFunction::PQ) ==
          Approx(1.0F).margin(0.01F));

    // And the top of the range is the whole 10000 cd/m2, a hundred times that.
    CHECK(render::toLinearScalar(1.0F, TransferFunction::PQ) == Approx(100.0F).margin(1.0F));
}

TEST_CASE("A log curve keeps highlights a display curve would have clipped",
          "[render][color][log]") {
    // The point of shooting log. Rec.709 runs out at 1.0; the log curves carry
    // several stops beyond it, and that headroom is what a grade is made from.
    for (const TransferFunction transfer :
         {TransferFunction::SLog3, TransferFunction::VLog, TransferFunction::LogC3}) {
        INFO(media::toString(transfer));
        CHECK(render::toLinearScalar(1.0F, transfer) > 4.0F);
    }
    CHECK(render::toLinearScalar(1.0F, TransferFunction::BT709) == Approx(1.0F).margin(0.01F));
}

TEST_CASE("The highlight rolloff is exactly the identity below its knee", "[render][tonemap]") {
    // The load-bearing property. A tone map that touched the midtones would
    // silently change every existing deliverable, and the first anybody would
    // know is a re-export not matching the one that was signed off.
    for (const float knee : {0.6F, 0.8F, 0.95F}) {
        for (const float value : {0.0F, 0.05F, 0.18F, 0.4F, 0.5F}) {
            if (value > knee) {
                continue;
            }
            INFO("knee " << knee << " value " << value);
            // Bit for bit, not approximately.
            CHECK(render::rolloff(value, knee) == value);
        }
        CHECK(render::rolloff(knee, knee) == knee);
    }
}

TEST_CASE("A knee of one or more is no rolloff at all", "[render][tonemap]") {
    // What this program did before there was a choice, and what an SDR project
    // with nothing above white wants anyway.
    for (const float value : {0.0F, 0.5F, 1.0F, 4.0F, 40.0F}) {
        CHECK(render::rolloff(value, 1.0F) == value);
        CHECK(render::rolloff(value, 2.0F) == value);
    }
}

TEST_CASE("Above the knee nothing clips and nothing collides", "[render][tonemap]") {
    constexpr float knee = 0.8F;
    float previous = render::rolloff(knee, knee);
    for (const float value : {0.9F, 1.0F, 1.5F, 2.0F, 5.0F, 20.0F, 100.0F}) {
        const float mapped = render::rolloff(value, knee);
        INFO("value " << value << " -> " << mapped);
        // Strictly increasing: two different highlights must not come out the
        // same value, or a sky becomes a flat patch.
        CHECK(mapped > previous);
        // And never reaching 1, so there is always somewhere left to go.
        CHECK(mapped < 1.0F);
        previous = mapped;
    }
}

TEST_CASE("The rolloff joins the identity without a corner", "[render][tonemap]") {
    // A discontinuity in the slope reads as a hard edge across a sky -- the one
    // artefact that makes a tone map worse than clipping.
    constexpr float knee = 0.75F;
    constexpr float step = 1e-4F;
    const float slopeBelow =
        (render::rolloff(knee - step, knee) - render::rolloff(knee - (2.0F * step), knee)) / step;
    const float slopeAbove =
        (render::rolloff(knee + (2.0F * step), knee) - render::rolloff(knee + step, knee)) / step;
    CHECK(slopeBelow == Approx(1.0F).margin(0.01F));
    CHECK(slopeAbove == Approx(1.0F).margin(0.01F));
}

TEST_CASE("Tone mapping an image works on straight colour", "[render][tonemap]") {
    // Premultiplied values would make the result depend on how transparent the
    // pixel is, so a highlight would tone map differently in the middle of a
    // dissolve than either side of it.
    render::RgbaImage image{2, 1};
    image.at(0, 0) = render::Rgba{4.0F, 4.0F, 4.0F, 1.0F};
    // The same colour at half coverage: premultiplied, that is 2.0.
    image.at(1, 0) = render::Rgba{2.0F, 2.0F, 2.0F, 0.5F};

    render::toneMap(image, 0.8F);
    CHECK(image.at(0, 0).r == Approx(image.at(1, 0).r * 2.0F).margin(0.001F));
    CHECK(image.at(0, 0).r < 1.0F);
    CHECK(image.at(1, 0).a == Approx(0.5F));

    SECTION("and leaves a frame with nothing above the knee alone") {
        render::RgbaImage ordinary{2, 1};
        ordinary.at(0, 0) = render::Rgba{0.2F, 0.4F, 0.6F, 1.0F};
        ordinary.at(1, 0) = render::Rgba{0.0F, 0.0F, 0.0F, 0.0F};
        const render::RgbaImage before = ordinary.clone();
        render::toneMap(ordinary, 0.8F);
        CHECK(ordinary.at(0, 0).r == before.at(0, 0).r);
        CHECK(ordinary.at(0, 0).g == before.at(0, 0).g);
        CHECK(ordinary.at(0, 0).b == before.at(0, 0).b);
    }
}

TEST_CASE("Tone mapping is what stops the encoder clipping", "[render][tonemap]") {
    // The encoder's job is to write what it is given and clip what does not
    // fit; making sure nothing needs clipping is a separate decision. This is
    // that division of labour, checked: the same frame, encoded with and
    // without the rolloff applied first.
    render::RgbaImage highlights{3, 1};
    highlights.at(0, 0) = render::Rgba{0.5F, 0.5F, 0.5F, 1.0F};  // below the knee
    highlights.at(1, 0) = render::Rgba{2.0F, 2.0F, 2.0F, 1.0F};
    highlights.at(2, 0) = render::Rgba{8.0F, 8.0F, 8.0F, 1.0F};

    std::vector<std::uint8_t> clipped(3 * 3);
    REQUIRE(render::toDisplayRgb24(highlights, clipped.data(), 3 * 3).ok());
    // Everything above white lands on the same code: two different highlights,
    // one value.
    CHECK(clipped[3] == 255);
    CHECK(clipped[6] == 255);

    render::RgbaImage rolled = highlights.clone();
    render::toneMap(rolled, 0.8F);
    std::vector<std::uint8_t> mapped(3 * 3);
    REQUIRE(render::toDisplayRgb24(rolled, mapped.data(), 3 * 3).ok());
    CHECK(mapped[3] < 255);
    CHECK(mapped[6] < 255);
    // Still ordered, and still distinct.
    CHECK(mapped[6] > mapped[3]);
    // And the pixel below the knee comes out on exactly the same code -- the
    // property the whole design rests on.
    CHECK(mapped[0] == clipped[0]);
}

TEST_CASE("A wide-gamut source is brought into the working space", "[render][color][gamut]") {
    // ADR-005 fixes the working space at Rec.709 primaries and says wider-gamut
    // sources are converted in. Until they were, a BT.2020 clip was composited
    // as though its numbers already meant Rec.709 -- oversaturated, and
    // disagreeing with a Rec.709 clip beside it in exactly the way that makes
    // shot matching fight the footage.
    //
    // A saturated red: Y low, Cr high. What it decodes to depends entirely on
    // what its primaries are said to be.
    ColorInfo wide = rec709Limited();
    wide.primaries = media::ColorPrimaries::BT2020;
    const media::VideoFrame wideFrame = flatFrame(81, 90, 240, wide);
    const media::VideoFrame narrowFrame = flatFrame(81, 90, 240, rec709Limited());

    render::RgbaImage wideLinear;
    render::RgbaImage narrowLinear;
    REQUIRE(render::toLinear(wideFrame, wideLinear));
    REQUIRE(render::toLinear(narrowFrame, narrowLinear));

    const render::Rgba wideRed = wideLinear.row(0)[0];
    const render::Rgba narrowRed = narrowLinear.row(0)[0];

    // The same numbers, read as two different gamuts, are two different
    // colours. If these matched, nothing would be converting.
    CHECK(std::fabs(wideRed.r - narrowRed.r) > 1e-3F);

    // BT.2020's red is outside Rec.709, so bringing it in puts green and blue
    // negative -- out of gamut, and honestly so, rather than silently clamped
    // to a less saturated red that was never in the footage.
    CHECK(wideRed.r > narrowRed.r);
    CHECK(wideRed.g < narrowRed.g);
}

TEST_CASE("A Rec.709 source is not touched by the gamut stage", "[render][color][gamut]") {
    // The identity path, which is most footage. This is also what keeps the
    // ADR's bit-identical round trip true: a clip that passes through untouched
    // must come out exactly as it went in, and a matrix multiply by an identity
    // that was computed in floats would not guarantee that.
    const media::VideoFrame frame = flatFrame(128, 100, 150, rec709Limited());
    render::RgbaImage linear;
    REQUIRE(render::toLinear(frame, linear));

    // Untagged primaries take the same path: an untagged source is composited
    // as if it were already in the working space, which is what happens today
    // and is the only answer that cannot make an existing project look
    // different.
    ColorInfo untagged = rec709Limited();
    untagged.primaries = media::ColorPrimaries::Unknown;
    const media::VideoFrame same = flatFrame(128, 100, 150, untagged);
    render::RgbaImage other;
    REQUIRE(render::toLinear(same, other));

    CHECK(linear.row(0)[0] == other.row(0)[0]);
}

TEST_CASE("Encoding with alpha keeps the coverage, straight", "[render][color]") {
    // For delivering a graphic over nothing -- a title, a lower third, a logo
    // -- which is the one case where compositing onto black at the encoder
    // throws away the only thing that made it worth exporting separately.
    render::RgbaImage image{2, 1};
    render::Rgba* row = image.row(0);
    // Premultiplied, as the compositor leaves it: a half-covered pixel whose
    // straight colour is white.
    row[0] = render::Rgba{0.5F, 0.5F, 0.5F, 0.5F};
    row[1] = render::Rgba{0.0F, 0.0F, 0.0F, 0.0F};

    std::vector<std::uint8_t> rgba(2 * 4, 0);
    REQUIRE(render::toDisplayRgba32(image, rgba.data(), 2 * 4));

    // The colour comes back straight -- white, not the half-grey it was stored
    // as -- because every format carrying an alpha channel expects it that way.
    CHECK(rgba[0] == 255);
    CHECK(rgba[1] == 255);
    CHECK(rgba[2] == 255);
    // And the coverage is linear: it is not a light level, so the transfer
    // curve has no business on it. Half coverage is 128, not the 188 a 2.2
    // gamma would make of it.
    CHECK(rgba[3] == 128);

    // Nothing there is transparent black rather than a division by zero.
    CHECK(rgba[4] == 0);
    CHECK(rgba[7] == 0);
}

TEST_CASE("Encoding with and without alpha agree about colour", "[render][color]") {
    // A graphic delivered both ways has to come back the same picture. The two
    // forms share one implementation for exactly this reason; the test is what
    // says they still do.
    render::RgbaImage image{4, 1};
    render::Rgba* row = image.row(0);
    for (std::int32_t x = 0; x < 4; ++x) {
        const float v = static_cast<float>(x) / 3.0F;
        row[x] = render::Rgba{v, v * 0.5F, 1.0F - v, 1.0F};
    }

    std::vector<std::uint8_t> three(4 * 3, 0);
    std::vector<std::uint8_t> four(4 * 4, 0);
    REQUIRE(render::toDisplayRgb24(image, three.data(), 4 * 3));
    REQUIRE(render::toDisplayRgba32(image, four.data(), 4 * 4));
    for (std::size_t x = 0; x < 4; ++x) {
        INFO("pixel " << x);
        CHECK(three[(x * 3) + 0] == four[(x * 4) + 0]);
        CHECK(three[(x * 3) + 1] == four[(x * 4) + 1]);
        CHECK(three[(x * 3) + 2] == four[(x * 4) + 2]);
        CHECK(four[(x * 4) + 3] == 255);
    }
}

TEST_CASE("An RGBA encode refuses a buffer sized for three components", "[render][color]") {
    render::RgbaImage image{4, 1};
    std::vector<std::uint8_t> tooSmall(4 * 3, 0);
    CHECK_FALSE(render::toDisplayRgba32(image, tooSmall.data(), 4 * 3));
}

// Packed RGB, which is how a still arrives.
//
// The regression these guard shipped black pictures. toLinear refused every
// pixel format that was not planar Y'CbCr, a .png decodes to rgb24 or rgba, and
// the render graph swallows an unreadable clip on purpose -- so an imported
// photograph composited as nothing at all and said nothing about it. The
// exported file was the right size and the right length and entirely empty.
TEST_CASE("Packed RGB converts to the working space", "[render][color][still]") {
    const std::int32_t size = 4;
    media::VideoFrame frame = media::VideoFrame::allocate(size, size, PixelFormat::RGB24);
    // Full range and no matrix, which is how every still decoder tags its
    // output: there is no Y'CbCr here to undo.
    frame.setColor(ColorInfo{media::ColorPrimaries::BT709, TransferFunction::BT709,
                             ColorMatrix::Identity, ColorRange::Full});
    for (std::int32_t row = 0; row < size; ++row) {
        std::uint8_t* pixels = frame.plane(0) + row * frame.stride(0);
        for (std::int32_t x = 0; x < size; ++x) {
            pixels[(x * 3) + 0] = 255;
            pixels[(x * 3) + 1] = 128;
            pixels[(x * 3) + 2] = 0;
        }
    }

    render::RgbaImage out;
    REQUIRE(render::toLinear(frame, out));
    const render::Rgba pixel = out.at(2, 2);

    // Linearised, not passed through: 128/255 encoded is about a fifth of the
    // light, which is the whole reason the working space exists.
    CHECK(pixel.r == Approx(1.0F).margin(1e-4));
    CHECK(pixel.g ==
          Approx(render::toLinearScalar(128.0F / 255.0F, TransferFunction::BT709)).margin(1e-4));
    CHECK(pixel.b == Approx(0.0F).margin(1e-4));
    // Opaque: RGB without an alpha channel covers what is beneath it.
    CHECK(pixel.a == Approx(1.0F).margin(1e-6));
}

TEST_CASE("A packed RGBA still keeps its transparency", "[render][color][still]") {
    // The reason alpha has to survive: a logo or a lower third arrives as an
    // RGBA .png, and the whole point of laying one over a shot is that the
    // shot shows through. Converting it to Y'CbCr on the way in -- the obvious
    // shortcut -- would drop the alpha and composite it onto black.
    const std::int32_t size = 2;
    media::VideoFrame frame = media::VideoFrame::allocate(size, size, PixelFormat::RGBA8);
    frame.setColor(ColorInfo{media::ColorPrimaries::BT709, TransferFunction::BT709,
                             ColorMatrix::Identity, ColorRange::Full});
    for (std::int32_t row = 0; row < size; ++row) {
        std::uint8_t* pixels = frame.plane(0) + row * frame.stride(0);
        for (std::int32_t x = 0; x < size; ++x) {
            pixels[(x * 4) + 0] = 255;
            pixels[(x * 4) + 1] = 255;
            pixels[(x * 4) + 2] = 255;
            // Opaque on the left, half covered on the right.
            pixels[(x * 4) + 3] = x == 0 ? 255 : 128;
        }
    }

    render::RgbaImage out;
    REQUIRE(render::toLinear(frame, out));
    CHECK(out.at(0, 0).a == Approx(1.0F).margin(1e-4));
    CHECK(out.at(1, 0).a == Approx(128.0F / 255.0F).margin(1e-4));
    // Straight, not premultiplied: the colour is untouched by the coverage.
    CHECK(out.at(1, 0).r == Approx(1.0F).margin(1e-4));
}
