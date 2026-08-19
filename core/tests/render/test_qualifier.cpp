#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/ColorPipeline.h"
#include "zaro/core/render/Qualifier.h"

using namespace zaro;
using Catch::Approx;

namespace {

render::QualifierConstants constantsFor(const model::HslQualifier& qualifier) {
    return render::qualifierConstantsFor(qualifier, media::TransferFunction::BT709);
}

float maskOf(const model::HslQualifier& qualifier, float r, float g, float b) {
    return render::qualifierMask(constantsFor(qualifier), r, g, b);
}

/// A linear value that shows at this display level.
float atDisplay(float level) {
    return render::toLinearScalar(level, media::TransferFunction::BT709);
}

}  // namespace

TEST_CASE("A disabled qualifier selects everything", "[render][qualifier]") {
    const model::HslQualifier off;
    CHECK_FALSE(off.enabled);
    CHECK(maskOf(off, 0.0F, 0.0F, 0.0F) == 1.0F);
    CHECK(maskOf(off, 0.8F, 0.1F, 0.1F) == 1.0F);
}

TEST_CASE("A qualifier left wide open still selects everything", "[render][qualifier]") {
    // Turning the qualifier on before narrowing it must not black out the
    // selection: the defaults are the whole hue circle and the whole range.
    model::HslQualifier wide;
    wide.enabled = true;
    CHECK(maskOf(wide, 0.5F, 0.5F, 0.5F) == Approx(1.0F));
    CHECK(maskOf(wide, 0.9F, 0.1F, 0.1F) == Approx(1.0F));
    CHECK(maskOf(wide, 0.0F, 0.0F, 0.0F) == Approx(1.0F));
}

TEST_CASE("Hue selection wraps around the circle", "[render][qualifier]") {
    // A window centred on red runs from about 350 to about 10. Subtracting
    // hues without wrapping says those are 340 degrees apart, and the
    // selection someone reaching for red would get is empty.
    model::HslQualifier reds;
    reds.enabled = true;
    reds.hueCentre = 0.0;
    reds.hueWidth = 40.0;
    reds.hueSoftness = 0.0;

    CHECK(maskOf(reds, 1.0F, 0.0F, 0.0F) == Approx(1.0F));   // hue 0
    CHECK(maskOf(reds, 1.0F, 0.15F, 0.0F) == Approx(1.0F));  // hue ~9, inside
    CHECK(maskOf(reds, 1.0F, 0.0F, 0.15F) == Approx(1.0F));  // hue ~351, inside
    CHECK(maskOf(reds, 0.0F, 1.0F, 0.0F) == Approx(0.0F));   // green, far away
    CHECK(maskOf(reds, 0.0F, 0.0F, 1.0F) == Approx(0.0F));   // blue, far away
}

TEST_CASE("A hue window covering the circle keeps neutral pixels", "[render][qualifier]") {
    // Grey has no hue: any answer is arbitrary. A qualifier meant to select
    // "everything dark" must not quietly drop the greys, which are most of
    // what is dark.
    model::HslQualifier darks;
    darks.enabled = true;
    darks.lumaHigh = 0.4;
    darks.lumaSoftness = 0.05;

    CHECK(maskOf(darks, 0.01F, 0.01F, 0.01F) == Approx(1.0F));
    CHECK(maskOf(darks, 0.9F, 0.9F, 0.9F) == Approx(0.0F));
}

TEST_CASE("Saturation windows select by how colourful a pixel is", "[render][qualifier]") {
    model::HslQualifier colourful;
    colourful.enabled = true;
    colourful.saturationLow = 0.5;
    colourful.saturationSoftness = 0.0;

    CHECK(maskOf(colourful, 1.0F, 0.0F, 0.0F) == Approx(1.0F));  // saturation 1
    CHECK(maskOf(colourful, 1.0F, 0.9F, 0.9F) == Approx(0.0F));  // nearly white
    CHECK(maskOf(colourful, 0.4F, 0.4F, 0.4F) == Approx(0.0F));  // grey

    model::HslQualifier pastel;
    pastel.enabled = true;
    pastel.saturationHigh = 0.3;
    pastel.saturationSoftness = 0.0;
    CHECK(pastel.enabled);
    CHECK(maskOf(pastel, 1.0F, 0.9F, 0.9F) == Approx(1.0F));
    CHECK(maskOf(pastel, 1.0F, 0.0F, 0.0F) == Approx(0.0F));
}

TEST_CASE("Luma thresholds are display-referred", "[render][qualifier]") {
    // "Midtones" means the tones that look like midtones. In linear light the
    // value that shows as half-way is under a fifth, and a qualifier that used
    // linear thresholds would select almost nothing anyone recognised.
    model::HslQualifier mids;
    mids.enabled = true;
    mids.lumaLow = 0.4;
    mids.lumaHigh = 0.6;
    mids.lumaSoftness = 0.0;

    const float middle = atDisplay(0.5F);
    CHECK(maskOf(mids, middle, middle, middle) == Approx(1.0F));
    // The *linear* midpoint shows far brighter than 0.6, so it is outside.
    CHECK(maskOf(mids, 0.5F, 0.5F, 0.5F) == Approx(0.0F));

    const float dark = atDisplay(0.2F);
    CHECK(maskOf(mids, dark, dark, dark) == Approx(0.0F));
}

TEST_CASE("A selection reaching white keeps everything above it", "[render][qualifier]") {
    // Linear light does not stop at white. A highlight three times white is
    // not "outside the highlights", and a qualifier that dropped it would
    // punch holes in exactly the region it was aimed at.
    model::HslQualifier highs;
    highs.enabled = true;
    highs.lumaLow = 0.7;
    highs.lumaSoftness = 0.05;

    CHECK(maskOf(highs, 1.0F, 1.0F, 1.0F) == Approx(1.0F));
    CHECK(maskOf(highs, 3.0F, 3.0F, 3.0F) == Approx(1.0F));
    CHECK(maskOf(highs, 40.0F, 40.0F, 40.0F) == Approx(1.0F));
}

TEST_CASE("Soft edges are smooth and monotonic", "[render][qualifier]") {
    // A hard threshold gives a mask with stepped edges, and a correction
    // through it looks like a sticker rather than a grade.
    model::HslQualifier soft;
    soft.enabled = true;
    soft.hueCentre = 120.0;  // green
    soft.hueWidth = 40.0;
    soft.hueSoftness = 40.0;

    // Sweep hue from green outwards and require the mask to fall without ever
    // rising again.
    float previous = 2.0F;
    bool sawPartial = false;
    for (int degrees = 120; degrees <= 200; degrees += 2) {
        const auto radians = static_cast<float>(degrees) * 3.14159265F / 180.0F;
        // A colour at this hue, fully saturated.
        const float sector = static_cast<float>(degrees) / 60.0F;
        const float x = 1.0F - std::fabs(std::fmod(sector, 2.0F) - 1.0F);
        float r = 0.0F;
        float g = 0.0F;
        float b = 0.0F;
        if (degrees < 180) {
            r = 0.0F;
            g = 1.0F;
            b = x;
        } else {
            r = 0.0F;
            g = x;
            b = 1.0F;
        }
        (void)radians;
        const float mask = maskOf(soft, r, g, b);
        CHECK(mask <= previous + 1e-5F);
        if (mask > 0.01F && mask < 0.99F) {
            sawPartial = true;
        }
        previous = mask;
    }
    CHECK(sawPartial);
}

TEST_CASE("Hue and saturation come from linear light", "[render][qualifier]") {
    float hue = 0.0F;
    float saturation = 0.0F;

    render::hueSaturationOf(1.0F, 0.0F, 0.0F, hue, saturation);
    CHECK(hue == Approx(0.0F));
    CHECK(saturation == Approx(1.0F));

    render::hueSaturationOf(0.0F, 1.0F, 0.0F, hue, saturation);
    CHECK(hue == Approx(120.0F));

    render::hueSaturationOf(0.0F, 0.0F, 1.0F, hue, saturation);
    CHECK(hue == Approx(240.0F));

    // Neutral has no hue, and no saturation at any brightness.
    for (const float grey : {0.0F, 0.2F, 1.0F, 5.0F}) {
        render::hueSaturationOf(grey, grey, grey, hue, saturation);
        CHECK(saturation == Approx(0.0F));
    }
}
