#pragma once

#include <array>

#include "zaro/core/media/ColorInfo.h"

namespace zaro::render {

/// A 3x3 conversion between two sets of colour primaries, row-major.
///
/// Linear light only. A gamut conversion is a change of basis between two sets
/// of real lights, and that is a linear operation on linear values -- applying
/// it to encoded ones mixes a matrix with a curve and produces something that
/// is neither.
struct GamutMatrix {
    /// Row-major: `m[row][column]`, so `out.r = m[0][0]*r + m[0][1]*g + ...`.
    std::array<std::array<float, 3>, 3> m{
        {{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}};

    /// Whether this is the identity, to within what a float can tell.
    ///
    /// Worth asking rather than always multiplying: most timelines are one
    /// gamut throughout, and the conversion there is three dot products per
    /// pixel that cannot change anything.
    [[nodiscard]] bool isIdentity() const noexcept;

    [[nodiscard]] std::array<float, 3> apply(float r, float g, float b) const noexcept {
        return {(m[0][0] * r) + (m[0][1] * g) + (m[0][2] * b),
                (m[1][0] * r) + (m[1][1] * g) + (m[1][2] * b),
                (m[2][0] * r) + (m[2][1] * g) + (m[2][2] * b)};
    }

    friend bool operator==(const GamutMatrix&, const GamutMatrix&) = default;
};

/// The matrix taking linear `from` primaries to linear `to` primaries.
///
/// Derived from the chromaticities rather than written down. The published
/// matrices are what this produces, and a table of them would be nine
/// transcribed digits per pair -- which is O(n^2) numbers to get right, and
/// silently wrong if one of them is mistyped. From the coordinates it is O(n),
/// each of which is a number quoted in the standard that defines it, and adding
/// a gamut is five pairs rather than a matrix against every existing one.
///
/// `Unknown` on either side is the identity: an untagged source is composited
/// as if it were already in the working space, which is what happens today and
/// is the only answer that cannot make an existing project look different.
///
/// **Every gamut here shares D65**, so no chromatic adaptation is involved --
/// see `whitePointOf`. DCI-P3 with its own white is the first one that would
/// need it, and it is deliberately not in the list yet: adding it means adding
/// Bradford adaptation with it, not quietly ignoring a white point difference
/// that would put a green cast on everything.
[[nodiscard]] GamutMatrix gamutMatrix(media::ColorPrimaries from, media::ColorPrimaries to);

/// The CIE xy chromaticities of a gamut: red, green, blue, then white.
///
/// Exposed because the matrices are derived from it and a test that checks the
/// matrices against published values should be able to say which coordinates
/// produced them.
struct Chromaticities {
    std::array<double, 2> red{};
    std::array<double, 2> green{};
    std::array<double, 2> blue{};
    std::array<double, 2> white{};
};

[[nodiscard]] Chromaticities chromaticitiesOf(media::ColorPrimaries primaries);

}  // namespace zaro::render
