// Gamut conversion: the matrices, checked against the numbers the standards
// publish rather than against themselves.
//
// `ColorPrimaries` has been probed, stored and displayed since Phase 1 and
// nothing ever converted between two of them, so BT.2020 and Display P3
// footage composited as if its primaries were the output's -- which is
// oversaturated, and which two clips from different cameras disagree about in
// exactly the way that makes shot matching fight the footage.

#include <array>
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/Gamut.h"

using namespace zaro;
using Catch::Approx;
using media::ColorPrimaries;

namespace {

/// Loose enough for the published matrices and tight enough that a wrong
/// primary is never within it.
///
/// The published values are quoted to four decimals and are derived with
/// whatever precision their author used for D65 -- 0.3127/0.3290 here, but
/// 0.31271/0.32902 elsewhere -- so the last quoted digit is not a fact about
/// the gamut. Agreement to better than a thousandth is the claim worth making;
/// getting a primary wrong moves a coefficient by hundredths at least, which
/// this catches with room to spare.
constexpr double kPublished = 5e-4;

}  // namespace

TEST_CASE("A gamut converted to itself is the identity", "[render][gamut]") {
    for (const ColorPrimaries primaries :
         {ColorPrimaries::BT709, ColorPrimaries::BT601_525, ColorPrimaries::BT601_625,
          ColorPrimaries::BT2020, ColorPrimaries::DisplayP3}) {
        CHECK(render::gamutMatrix(primaries, primaries).isIdentity());
    }
}

TEST_CASE("An untagged gamut is left alone", "[render][gamut]") {
    // Any other answer is a guess about somebody's footage that changes how an
    // existing project looks.
    CHECK(render::gamutMatrix(ColorPrimaries::Unknown, ColorPrimaries::BT2020).isIdentity());
    CHECK(render::gamutMatrix(ColorPrimaries::BT2020, ColorPrimaries::Unknown).isIdentity());
}

TEST_CASE("BT.2020 to BT.709 matches the published matrix", "[render][gamut]") {
    // ITU-R BT.2087 / the widely published conversion. If this drifts, the
    // chromaticities above it are wrong, not this.
    const render::GamutMatrix m =
        render::gamutMatrix(ColorPrimaries::BT2020, ColorPrimaries::BT709);
    CHECK(m.m[0][0] == Approx(1.6605).margin(kPublished));
    CHECK(m.m[0][1] == Approx(-0.5876).margin(kPublished));
    CHECK(m.m[0][2] == Approx(-0.0728).margin(kPublished));
    CHECK(m.m[1][0] == Approx(-0.1246).margin(kPublished));
    CHECK(m.m[1][1] == Approx(1.1329).margin(kPublished));
    CHECK(m.m[1][2] == Approx(-0.0083).margin(kPublished));
    CHECK(m.m[2][0] == Approx(-0.0182).margin(kPublished));
    CHECK(m.m[2][1] == Approx(-0.1006).margin(kPublished));
    CHECK(m.m[2][2] == Approx(1.1187).margin(kPublished));
}

TEST_CASE("Display P3 to BT.709 matches the published matrix", "[render][gamut]") {
    const render::GamutMatrix m =
        render::gamutMatrix(ColorPrimaries::DisplayP3, ColorPrimaries::BT709);
    CHECK(m.m[0][0] == Approx(1.2249).margin(kPublished));
    CHECK(m.m[0][1] == Approx(-0.2247).margin(kPublished));
    CHECK(m.m[0][2] == Approx(0.0).margin(kPublished));
    CHECK(m.m[1][0] == Approx(-0.0420).margin(kPublished));
    CHECK(m.m[1][1] == Approx(1.0419).margin(kPublished));
    CHECK(m.m[1][2] == Approx(0.0).margin(kPublished));
    CHECK(m.m[2][0] == Approx(-0.0197).margin(kPublished));
    CHECK(m.m[2][1] == Approx(-0.0786).margin(kPublished));
    CHECK(m.m[2][2] == Approx(1.0983).margin(kPublished));
}

TEST_CASE("White stays white through every conversion", "[render][gamut]") {
    // The property the scaling step exists for. Without it the matrix maps the
    // right hues and the wrong brightnesses, and white comes out tinted --
    // which looks like a bug in something else entirely.
    //
    // True for every pair here because they all share D65. The first gamut
    // with its own white point will need chromatic adaptation, and this is the
    // test that will say so.
    for (const ColorPrimaries from :
         {ColorPrimaries::BT709, ColorPrimaries::BT601_525, ColorPrimaries::BT601_625,
          ColorPrimaries::BT2020, ColorPrimaries::DisplayP3}) {
        for (const ColorPrimaries to :
             {ColorPrimaries::BT709, ColorPrimaries::BT601_525, ColorPrimaries::BT601_625,
              ColorPrimaries::BT2020, ColorPrimaries::DisplayP3}) {
            const auto white = render::gamutMatrix(from, to).apply(1.0F, 1.0F, 1.0F);
            INFO("from " << media::toString(from) << " to " << media::toString(to));
            CHECK(white[0] == Approx(1.0F).margin(1e-4));
            CHECK(white[1] == Approx(1.0F).margin(1e-4));
            CHECK(white[2] == Approx(1.0F).margin(1e-4));
        }
    }
}

TEST_CASE("Every conversion is reversible", "[render][gamut]") {
    // A change of basis between three real lights, so there and back is the
    // identity to within float. A matrix that failed this would be losing
    // colour on every round trip through a nested sequence.
    for (const ColorPrimaries from :
         {ColorPrimaries::BT709, ColorPrimaries::BT601_525, ColorPrimaries::BT2020,
          ColorPrimaries::DisplayP3}) {
        for (const ColorPrimaries to :
             {ColorPrimaries::BT709, ColorPrimaries::BT601_625, ColorPrimaries::BT2020,
              ColorPrimaries::DisplayP3}) {
            const render::GamutMatrix there = render::gamutMatrix(from, to);
            const render::GamutMatrix back = render::gamutMatrix(to, from);
            for (const std::array<float, 3>& colour : {std::array<float, 3>{0.2F, 0.7F, 0.4F},
                                                       std::array<float, 3>{1.0F, 0.0F, 0.0F},
                                                       std::array<float, 3>{0.05F, 0.05F, 0.9F}}) {
                const auto once = there.apply(colour[0], colour[1], colour[2]);
                const auto twice = back.apply(once[0], once[1], once[2]);
                INFO("from " << media::toString(from) << " to " << media::toString(to));
                CHECK(twice[0] == Approx(colour[0]).margin(1e-5));
                CHECK(twice[1] == Approx(colour[1]).margin(1e-5));
                CHECK(twice[2] == Approx(colour[2]).margin(1e-5));
            }
        }
    }
}

TEST_CASE("A wide gamut narrows when it is brought to a small one",
          "[render][gamut]") {
    // The reason any of this exists. BT.2020's red is far outside BT.709, so
    // saturated BT.2020 red has no BT.709 equivalent and comes back with a
    // negative green and blue -- out of gamut, and honestly so. Composited
    // without the conversion it would simply have been BT.709's red: a
    // different, less saturated colour, silently.
    const auto red = render::gamutMatrix(ColorPrimaries::BT2020, ColorPrimaries::BT709)
                         .apply(1.0F, 0.0F, 0.0F);
    CHECK(red[0] > 1.0F);
    CHECK(red[1] < 0.0F);
    CHECK(red[2] < 0.0F);

    // And the other way: every BT.709 colour fits inside BT.2020, so nothing
    // goes negative.
    const auto inside = render::gamutMatrix(ColorPrimaries::BT709, ColorPrimaries::BT2020)
                            .apply(1.0F, 0.0F, 0.0F);
    CHECK(inside[0] > 0.0F);
    CHECK(inside[1] >= 0.0F);
    CHECK(inside[2] >= 0.0F);
}
