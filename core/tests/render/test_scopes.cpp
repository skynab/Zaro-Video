#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/ColorPipeline.h"
#include "zaro/core/render/Scopes.h"

using namespace zaro;
using Catch::Approx;
using render::Rgba;
using render::RgbaImage;

namespace {

RgbaImage solid(std::int32_t width, std::int32_t height, float r, float g, float b,
                float a = 1.0F) {
    RgbaImage image{width, height};
    // Premultiplied, the same as everything else in the working space.
    image.fill(Rgba{r * a, g * a, b * a, a});
    return image;
}

/// The level a linear value should land on, worked out through the encoder
/// rather than assumed, so the test is checking the scope's binning and not
/// re-stating its arithmetic.
std::int32_t expectedLevel(float linear,
                           media::TransferFunction transfer = media::TransferFunction::BT709) {
    const float encoded = render::fromLinearScalar(linear, transfer);
    return std::clamp(static_cast<std::int32_t>((encoded * 255.0F) + 0.5F), 0, 255);
}

/// The single level a whole-frame waveform has any counts on, or -1 if it is
/// spread over several.
std::int32_t onlyLevel(const render::Waveform& waveform) {
    std::int32_t found = -1;
    for (std::int32_t column = 0; column < waveform.columns(); ++column) {
        for (std::int32_t level = 0; level < render::Waveform::kLevels; ++level) {
            if (waveform.at(column, level) == 0) {
                continue;
            }
            if (found != -1 && found != level) {
                return -1;
            }
            found = level;
        }
    }
    return found;
}

}  // namespace

TEST_CASE("A flat frame reads as one level across the whole waveform", "[render][scopes]") {
    const RgbaImage white = solid(64, 32, 1.0F, 1.0F, 1.0F);
    const auto scopes = render::measure(white);

    CHECK(onlyLevel(scopes.luma) == 255);
    CHECK(onlyLevel(scopes.red) == 255);
    CHECK(onlyLevel(scopes.blue) == 255);

    const RgbaImage black = solid(64, 32, 0.0F, 0.0F, 0.0F);
    CHECK(onlyLevel(render::measure(black).luma) == 0);
}

TEST_CASE("Scopes measure the display signal, not the linear working space", "[render][scopes]") {
    // The point of the whole design. In linear light, 18% grey reads as 18;
    // through the transfer function it reads near 46, which is where every
    // reference a colourist works against expects it.
    const RgbaImage grey = solid(32, 32, 0.18F, 0.18F, 0.18F);
    const auto scopes = render::measure(grey);

    const std::int32_t level = onlyLevel(scopes.luma);
    CHECK(level == expectedLevel(0.18F));
    CHECK(level > 100);  // nowhere near the linear value of 46/255
    CHECK(level < 140);

    // And a scope reading depends on the curve the frame is shown through,
    // which is the honest consequence: the instrument measures what goes out.
    render::ScopeOptions asIs;
    asIs.transfer = media::TransferFunction::Linear;
    CHECK(onlyLevel(render::measure(grey, asIs).luma) ==
          expectedLevel(0.18F, media::TransferFunction::Linear));
}

TEST_CASE("A horizontal ramp reads as a diagonal waveform", "[render][scopes]") {
    RgbaImage ramp{256, 16};
    for (std::int32_t y = 0; y < ramp.height(); ++y) {
        Rgba* row = ramp.row(y);
        for (std::int32_t x = 0; x < ramp.width(); ++x) {
            const float value = static_cast<float>(x) / 255.0F;
            row[x] = Rgba{value, value, value, 1.0F};
        }
    }

    render::ScopeOptions options;
    options.waveformColumns = 256;
    const auto scopes = render::measure(ramp, options);

    // Each column holds exactly one level, and the levels only ever climb --
    // which is what "the waveform of a ramp is a diagonal" means as an
    // assertion rather than as a picture.
    std::int32_t previous = -1;
    for (std::int32_t column = 0; column < 256; ++column) {
        std::int32_t levels = 0;
        std::int32_t level = -1;
        for (std::int32_t candidate = 0; candidate < render::Waveform::kLevels; ++candidate) {
            if (scopes.luma.at(column, candidate) > 0) {
                ++levels;
                level = candidate;
            }
        }
        REQUIRE(levels == 1);
        CHECK(level >= previous);
        previous = level;
    }
    CHECK(previous == 255);
}

TEST_CASE("The histogram counts every pixel once per channel", "[render][scopes]") {
    const RgbaImage frame = solid(40, 20, 1.0F, 0.0F, 0.0F);
    const auto scopes = render::measure(frame);

    std::uint64_t total = 0;
    for (std::uint32_t count : scopes.histogram.red) {
        total += count;
    }
    CHECK(total == 40U * 20U);
    CHECK(scopes.histogram.red[255] == 40U * 20U);
    CHECK(scopes.histogram.green[0] == 40U * 20U);
    CHECK(scopes.histogram.peak == 40U * 20U);
}

TEST_CASE("Neutral lands at the centre of the vectorscope", "[render][scopes]") {
    for (const float grey : {0.0F, 0.18F, 0.5F, 1.0F}) {
        const auto scopes = render::measure(solid(16, 16, grey, grey, grey));
        const std::int32_t centre = scopes.vectorscope.size() / 2;
        // Every pixel on the centre point: no chroma at any brightness.
        CHECK(scopes.vectorscope.at(centre, centre) == 16U * 16U);
    }
}

TEST_CASE("The primaries land in the directions a graticule expects", "[render][scopes]") {
    constexpr std::int32_t kSize = 256;
    const float half = kSize / 2.0F;
    const auto plot = [](float r, float g, float b) {
        float x = 0.0F;
        float y = 0.0F;
        render::Vectorscope::plotFor(r, g, b, kSize, x, y);
        return std::pair{x, y};
    };

    const auto [redX, redY] = plot(1.0F, 0.0F, 0.0F);
    const auto [greenX, greenY] = plot(0.0F, 1.0F, 0.0F);
    const auto [blueX, blueY] = plot(0.0F, 0.0F, 1.0F);

    // Red is up and left of centre in Cb, green down and left, blue up-left in
    // Cr terms -- stated as signs rather than angles so this is a check on the
    // orientation rather than a restatement of the formula.
    CHECK(redY < half);  // Cr positive, drawn upwards
    CHECK(redX < half);  // Cb negative
    CHECK(greenY > half);
    CHECK(greenX < half);
    CHECK(blueY > half);
    CHECK(blueX > half);

    // Complements sit opposite their primaries, which is the property that
    // makes the graticule readable at all.
    const auto [cyanX, cyanY] = plot(0.0F, 1.0F, 1.0F);
    CHECK(cyanX == Approx(kSize - redX).margin(0.01));
    CHECK(cyanY == Approx(kSize - redY).margin(0.01));
}

TEST_CASE("A less saturated colour sits closer to the centre", "[render][scopes]") {
    constexpr std::int32_t kSize = 256;
    const float half = kSize / 2.0F;
    const auto radius = [half](float r, float g, float b) {
        float x = 0.0F;
        float y = 0.0F;
        render::Vectorscope::plotFor(r, g, b, kSize, x, y);
        return std::hypot(x - half, y - half);
    };

    CHECK(radius(1.0F, 0.0F, 0.0F) > radius(0.75F, 0.25F, 0.25F));
    CHECK(radius(0.75F, 0.25F, 0.25F) > radius(0.5F, 0.4F, 0.4F));
    CHECK(radius(0.5F, 0.5F, 0.5F) == Approx(0.0F).margin(1e-4));
}

TEST_CASE("A faded clip reads as unchanged brightness, not as a darker one", "[render][scopes]") {
    // The working space is premultiplied, so a half-transparent white pixel is
    // stored as 0.5. Measuring that directly would report a dissolve as a
    // change in exposure, and a colourist would chase an exposure problem that
    // is really an opacity.
    const RgbaImage opaque = solid(16, 16, 1.0F, 1.0F, 1.0F, 1.0F);
    const RgbaImage faded = solid(16, 16, 1.0F, 1.0F, 1.0F, 0.5F);

    CHECK(onlyLevel(render::measure(opaque).luma) == 255);
    CHECK(onlyLevel(render::measure(faded).luma) == 255);
}

TEST_CASE("Sampling every Nth row does not change the shape of the waveform", "[render][scopes]") {
    RgbaImage ramp{128, 64};
    for (std::int32_t y = 0; y < ramp.height(); ++y) {
        Rgba* row = ramp.row(y);
        for (std::int32_t x = 0; x < ramp.width(); ++x) {
            const float value = static_cast<float>(x) / 127.0F;
            row[x] = Rgba{value, value, value, 1.0F};
        }
    }

    render::ScopeOptions full;
    full.waveformColumns = 128;
    render::ScopeOptions sparse = full;
    sparse.rowStride = 4;

    const auto dense = render::measure(ramp, full);
    const auto thin = render::measure(ramp, sparse);
    // Same levels occupied, a quarter of the counts.
    for (std::int32_t column = 0; column < 128; ++column) {
        for (std::int32_t level = 0; level < render::Waveform::kLevels; ++level) {
            CHECK((dense.luma.at(column, level) > 0) == (thin.luma.at(column, level) > 0));
        }
    }
    CHECK(thin.luma.peak() * 4 == dense.luma.peak());
}

TEST_CASE("An empty frame measures to empty scopes rather than crashing", "[render][scopes]") {
    const auto scopes = render::measure(RgbaImage{});
    CHECK(scopes.luma.peak() == 0);
    CHECK(scopes.histogram.isValid());
    CHECK(scopes.histogram.peak == 0);
    CHECK(scopes.vectorscope.peak() == 0);
}
