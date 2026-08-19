#include <cmath>
#include <sstream>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/io/CubeLut.h"

using namespace zaro;
using Catch::Approx;

namespace {

/// An identity 3D LUT of the given size, written in the format's own order:
/// red fastest, then green, then blue.
std::string identityCube(int size) {
    std::ostringstream out;
    out << "TITLE \"identity\"\n";
    out << "LUT_3D_SIZE " << size << "\n";
    for (int b = 0; b < size; ++b) {
        for (int g = 0; g < size; ++g) {
            for (int r = 0; r < size; ++r) {
                out << (static_cast<double>(r) / (size - 1)) << " "
                    << (static_cast<double>(g) / (size - 1)) << " "
                    << (static_cast<double>(b) / (size - 1)) << "\n";
            }
        }
    }
    return out.str();
}

/// A LUT that swaps red and blue. Distinguishable from the identity in a way
/// that a symmetric look would not be -- which is the whole point, since the
/// classic .cube bug is reading the axes in the wrong order.
std::string swapCube(int size) {
    std::ostringstream out;
    out << "LUT_3D_SIZE " << size << "\n";
    for (int b = 0; b < size; ++b) {
        for (int g = 0; g < size; ++g) {
            for (int r = 0; r < size; ++r) {
                out << (static_cast<double>(b) / (size - 1)) << " "
                    << (static_cast<double>(g) / (size - 1)) << " "
                    << (static_cast<double>(r) / (size - 1)) << "\n";
            }
        }
    }
    return out.str();
}

}  // namespace

TEST_CASE("An identity cube leaves colours alone", "[io][lut]") {
    const auto lut = io::CubeLut::parse(identityCube(17));
    REQUIRE(lut);
    CHECK(lut->size() == 17);
    CHECK(lut->title() == "identity");
    CHECK(lut->shape() == io::CubeLut::Shape::ThreeD);

    for (const auto& colour : {std::array{0.0F, 0.0F, 0.0F}, std::array{1.0F, 1.0F, 1.0F},
                               std::array{0.25F, 0.5F, 0.75F}, std::array{0.9F, 0.1F, 0.4F}}) {
        float r = colour[0];
        float g = colour[1];
        float b = colour[2];
        lut->apply(r, g, b);
        CHECK(r == Approx(colour[0]).margin(1e-5));
        CHECK(g == Approx(colour[1]).margin(1e-5));
        CHECK(b == Approx(colour[2]).margin(1e-5));
    }
}

TEST_CASE("The red index moves fastest", "[io][lut]") {
    // Reading the axes in the wrong order swaps red and blue in every look,
    // which reads as a colour problem rather than an indexing one. A LUT that
    // deliberately swaps them is the only way to tell the two apart.
    const auto lut = io::CubeLut::parse(swapCube(9));
    REQUIRE(lut);

    float r = 0.8F;
    float g = 0.3F;
    float b = 0.1F;
    lut->apply(r, g, b);
    CHECK(r == Approx(0.1F).margin(1e-5));
    CHECK(g == Approx(0.3F).margin(1e-5));
    CHECK(b == Approx(0.8F).margin(1e-5));
}

TEST_CASE("A 1D cube maps each channel through its own curve", "[io][lut]") {
    std::ostringstream text;
    text << "LUT_1D_SIZE 3\n";
    text << "0.0 0.0 0.0\n";
    text << "0.25 0.5 0.75\n";
    text << "1.0 1.0 1.0\n";

    const auto lut = io::CubeLut::parse(text.str());
    REQUIRE(lut);
    CHECK(lut->shape() == io::CubeLut::Shape::OneD);

    float r = 0.5F;
    float g = 0.5F;
    float b = 0.5F;
    lut->apply(r, g, b);
    CHECK(r == Approx(0.25F).margin(1e-5));
    CHECK(g == Approx(0.5F).margin(1e-5));
    CHECK(b == Approx(0.75F).margin(1e-5));
}

TEST_CASE("The domain is honoured and inputs outside it are clamped", "[io][lut]") {
    // A log LUT that ignored its own domain would be reading the wrong part of
    // its table, and extrapolating past it produces colours nobody chose.
    std::ostringstream text;
    text << "LUT_1D_SIZE 2\n";
    text << "DOMAIN_MIN 0.0 0.0 0.0\n";
    text << "DOMAIN_MAX 2.0 2.0 2.0\n";
    text << "0.0 0.0 0.0\n";
    text << "1.0 1.0 1.0\n";

    const auto lut = io::CubeLut::parse(text.str());
    REQUIRE(lut);
    CHECK(lut->domainMax()[0] == Approx(2.0F));

    float r = 1.0F;
    float g = 1.0F;
    float b = 1.0F;
    lut->apply(r, g, b);
    // Half way through a domain that runs to two.
    CHECK(r == Approx(0.5F).margin(1e-5));

    r = 5.0F;
    g = 5.0F;
    b = 5.0F;
    lut->apply(r, g, b);
    CHECK(r == Approx(1.0F).margin(1e-5));

    r = -3.0F;
    g = -3.0F;
    b = -3.0F;
    lut->apply(r, g, b);
    CHECK(r == Approx(0.0F).margin(1e-5));
}

TEST_CASE("Comments and blank lines are ignored", "[io][lut]") {
    std::ostringstream text;
    text << "# a comment\n";
    text << "\n";
    text << "TITLE \"spaced out\"\n";
    text << "LUT_1D_SIZE 2   # trailing comment\n";
    text << "  0.0 0.0 0.0\n";
    text << "\n";
    text << "1.0 1.0 1.0\n";

    const auto lut = io::CubeLut::parse(text.str());
    REQUIRE(lut);
    CHECK(lut->title() == "spaced out");
    CHECK(lut->size() == 2);
}

TEST_CASE("A malformed cube is refused rather than half read", "[io][lut]") {
    // Every one of these produces a LUT that would otherwise look plausible and
    // grade wrongly, which is worse than not loading.
    CHECK_FALSE(io::CubeLut::parse("0.0 0.0 0.0\n"));                 // data before the size
    CHECK_FALSE(io::CubeLut::parse("LUT_3D_SIZE 2\n0.0 0.0 0.0\n"));  // too few entries
    CHECK_FALSE(io::CubeLut::parse("LUT_3D_SIZE 1\n"));               // a size of one
    CHECK_FALSE(io::CubeLut::parse("TITLE \"no size\"\n"));           // no size at all
    CHECK_FALSE(
        io::CubeLut::parse("LUT_1D_SIZE 2\nDOMAIN_MIN 1 1 1\nDOMAIN_MAX 0 0 0\n"
                           "0 0 0\n1 1 1\n"));  // inverted domain

    std::ostringstream tooMany;
    tooMany << "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n0.5 0.5 0.5\n";
    CHECK_FALSE(io::CubeLut::parse(tooMany.str()));
}

TEST_CASE("A missing file is an error, not an empty LUT", "[io][lut]") {
    CHECK_FALSE(io::CubeLut::load("/definitely/not/a/lut.cube"));
}

TEST_CASE("Interpolation is smooth between entries", "[io][lut]") {
    // A coarse LUT with a strong look: the point is that stepping through it
    // never jumps, since a visible step in a look is a banded picture.
    std::ostringstream text;
    const int size = 3;
    text << "LUT_3D_SIZE " << size << "\n";
    for (int b = 0; b < size; ++b) {
        for (int g = 0; g < size; ++g) {
            for (int r = 0; r < size; ++r) {
                const double rr = static_cast<double>(r) / (size - 1);
                text << (rr * rr) << " " << (static_cast<double>(g) / (size - 1)) << " "
                     << (static_cast<double>(b) / (size - 1)) << "\n";
            }
        }
    }
    const auto lut = io::CubeLut::parse(text.str());
    REQUIRE(lut);

    bool first = true;
    float previous = 0.0F;
    for (int i = 0; i <= 200; ++i) {
        float r = static_cast<float>(i) / 200.0F;
        float g = 0.5F;
        float b = 0.5F;
        lut->apply(r, g, b);
        if (!first) {
            CHECK(r >= previous - 1e-6F);
            CHECK(r - previous < 0.05F);  // no jumps
        }
        first = false;
        previous = r;
    }
}
