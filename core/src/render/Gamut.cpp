#include "zaro/core/render/Gamut.h"

#include <cmath>

namespace zaro::render {
namespace {

using media::ColorPrimaries;

using Matrix3 = std::array<std::array<double, 3>, 3>;

/// D65, as every gamut in `ColorPrimaries` uses it. See `gamutMatrix`.
constexpr std::array<double, 2> kD65{0.3127, 0.3290};

Matrix3 multiply(const Matrix3& a, const Matrix3& b) {
    Matrix3 out{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) {
                sum += a[static_cast<std::size_t>(row)][static_cast<std::size_t>(k)] *
                       b[static_cast<std::size_t>(k)][static_cast<std::size_t>(column)];
            }
            out[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)] = sum;
        }
    }
    return out;
}

/// The inverse, by cofactors. Three by three, and every matrix here is a change
/// of basis between three real lights, so the determinant cannot be zero for
/// any gamut whose primaries are actually distinct.
Matrix3 inverse(const Matrix3& m) {
    const double a = m[0][0];
    const double b = m[0][1];
    const double c = m[0][2];
    const double d = m[1][0];
    const double e = m[1][1];
    const double f = m[1][2];
    const double g = m[2][0];
    const double h = m[2][1];
    const double i = m[2][2];

    const double determinant =
        (a * ((e * i) - (f * h))) - (b * ((d * i) - (f * g))) + (c * ((d * h) - (e * g)));
    if (std::abs(determinant) < 1e-12) {
        return Matrix3{{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}}};
    }
    const double s = 1.0 / determinant;
    return Matrix3{{{((e * i) - (f * h)) * s, ((c * h) - (b * i)) * s, ((b * f) - (c * e)) * s},
                    {((f * g) - (d * i)) * s, ((a * i) - (c * g)) * s, ((c * d) - (a * f)) * s},
                    {((d * h) - (e * g)) * s, ((b * g) - (a * h)) * s, ((a * e) - (b * d)) * s}}};
}

/// A chromaticity as a tristimulus with Y = 1.
std::array<double, 3> tristimulus(const std::array<double, 2>& xy) {
    const double x = xy[0];
    const double y = xy[1];
    return {x / y, 1.0, (1.0 - x - y) / y};
}

/// The matrix taking linear RGB in these primaries to CIE XYZ.
///
/// The standard construction: the three primaries as columns, scaled so that
/// RGB (1,1,1) lands exactly on the white point. Without that scaling the
/// matrix maps the right hues and the wrong brightnesses, and white comes out
/// tinted -- which is the failure that looks like a bug in something else.
Matrix3 toXyz(const Chromaticities& c) {
    const auto r = tristimulus(c.red);
    const auto g = tristimulus(c.green);
    const auto b = tristimulus(c.blue);
    const Matrix3 primaries{{{r[0], g[0], b[0]}, {r[1], g[1], b[1]}, {r[2], g[2], b[2]}}};

    const auto white = tristimulus(c.white);
    const Matrix3 inverted = inverse(primaries);
    std::array<double, 3> scale{};
    for (int row = 0; row < 3; ++row) {
        const auto i = static_cast<std::size_t>(row);
        scale[i] = (inverted[i][0] * white[0]) + (inverted[i][1] * white[1]) +
                   (inverted[i][2] * white[2]);
    }
    Matrix3 out = primaries;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            out[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)] *=
                scale[static_cast<std::size_t>(column)];
        }
    }
    return out;
}

}  // namespace

Chromaticities chromaticitiesOf(ColorPrimaries primaries) {
    switch (primaries) {
        case ColorPrimaries::BT601_525:
            // SMPTE 170M / SMPTE-C, the NTSC-derived set actually used after
            // 1979 -- not the 1953 NTSC primaries, which no equipment ever
            // really had and which would turn every SD tape green.
            return {{0.630, 0.340}, {0.310, 0.595}, {0.155, 0.070}, kD65};
        case ColorPrimaries::BT601_625:
            // BT.470BG. Red and blue are BT.709's; only green differs, which is
            // why PAL SD composited as HD is subtly off rather than obviously.
            return {{0.640, 0.330}, {0.290, 0.600}, {0.150, 0.060}, kD65};
        case ColorPrimaries::BT2020:
            return {{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}, kD65};
        case ColorPrimaries::DisplayP3:
            // Display P3, not DCI-P3: the same primaries, D65 rather than DCI
            // white. This is what a phone and a modern laptop record and show.
            return {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, kD65};
        case ColorPrimaries::BT709:
        case ColorPrimaries::Unknown:
        default:
            return {{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}, kD65};
    }
}

bool GamutMatrix::isIdentity() const noexcept {
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const float wanted = row == column ? 1.0F : 0.0F;
            if (std::abs(m[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)] -
                         wanted) > 1e-6F) {
                return false;
            }
        }
    }
    return true;
}

GamutMatrix gamutMatrix(ColorPrimaries from, ColorPrimaries to) {
    // Untagged is composited as if it were already in the working space. Any
    // other answer would be a guess about somebody's footage that changes how
    // an existing project looks.
    if (from == ColorPrimaries::Unknown || to == ColorPrimaries::Unknown || from == to) {
        return GamutMatrix{};
    }
    const Matrix3 sourceToXyz = toXyz(chromaticitiesOf(from));
    const Matrix3 xyzToTarget = inverse(toXyz(chromaticitiesOf(to)));
    const Matrix3 combined = multiply(xyzToTarget, sourceToXyz);

    GamutMatrix out;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            out.m[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)] =
                static_cast<float>(
                    combined[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)]);
        }
    }
    return out;
}

}  // namespace zaro::render
