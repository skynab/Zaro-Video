#pragma once

#include <cstdint>
#include <vector>

namespace zaro::model {

/// One point on a tone curve, in the display-encoded domain.
///
/// Both coordinates are 0..1, where 0 is black and 1 is white *as the signal is
/// shown*, not as it is stored. See [ADR-012](docs/adr/0012): a curve's axes
/// are the ones the scopes read and the ones a colourist has in mind, which are
/// not the linear ones the compositor works in.
struct CurvePoint {
    double x{0.0};
    double y{0.0};

    friend bool operator==(const CurvePoint&, const CurvePoint&) = default;
};

/// A tone curve: control points and the interpolation between them.
///
/// Empty means identity, and identity costs nothing — no table is built and no
/// pixel is touched. That matters because four of these exist per clip and
/// almost every clip uses none of them.
class ToneCurve {
public:
    /// Fewer than two points cannot describe a mapping, so a curve with one
    /// point is still identity rather than a constant.
    [[nodiscard]] bool isIdentity() const;
    [[nodiscard]] const std::vector<CurvePoint>& points() const noexcept { return points_; }
    [[nodiscard]] std::size_t size() const noexcept { return points_.size(); }

    /// Add a point, or replace the one already at that x.
    ///
    /// Two points at one x describe a vertical segment, which is not a
    /// function, and every evaluation would have to pick one of them.
    void set(const CurvePoint& point);
    bool removeAt(double x, double tolerance = 1e-6);
    void clear() { points_.clear(); }

    /// The curve's value at `x`, both in the encoded domain.
    ///
    /// Interpolation is a monotonic cubic (Fritsch–Carlson). A plain cubic
    /// spline through the same points overshoots near a steep segment, and an
    /// overshooting tone curve is not a subtle error: it puts a dark halo above
    /// a highlight and can invert a gradient. Monotonic means the curve never
    /// reverses direction between two points that do not.
    [[nodiscard]] double valueAt(double x) const;

    friend bool operator==(const ToneCurve&, const ToneCurve&) = default;

private:
    /// Sorted by x, no duplicates.
    std::vector<CurvePoint> points_;
};

/// Curves that reshape colour, differing in what they are indexed by and in
/// what they do with the answer.
///
/// A different kind of curve from the tone ones below, sharing their machinery
/// and almost none of their meaning:
///
///   * **0.5 is neutral, not 0.** The value is a multiplier and the curve has
///     to be able to go both ways, so the middle of the range means "leave it
///     alone": 0 removes the colour, 0.5 keeps it, 1 doubles it. A curve with
///     no points is identity, as the tone curves are, which is not the same as
///     a flat curve at 0.5 -- that is the identity spelled out, and it still
///     costs a table.
///   * **The two saturation curves multiply together**, and with the primary
///     saturation, because all three answer one question: how far from grey
///     should this pixel be. Pulling the blues down and the shadows down should
///     compound where a pixel is both, which is what a product does and what
///     two sequential lerps toward luma would not. The hue shift is a different
///     question and is applied on its own.
///
/// Held as one struct because they are one control surface -- the curve editor
/// offers them in the same list -- and because a clip carrying three separate
/// single-member structs would be three fields to serialise and three
/// operations to undo separately when they are one gesture apiece.
struct ColorCurves {
    /// Against hue. **x wraps**: hue is a circle, so 0 and 1 are the same
    /// place, and a curve evaluated without wrapping has a seam at red -- the
    /// single most common hue anybody adjusts. `render::ColorCurveTable` is
    /// where the wrap happens, once, at bake time.
    ///
    /// Pull the sky down without touching skin, lift a tired green, take the
    /// ring out of a magenta practical.
    ToneCurve againstHue;

    /// Against brightness, on the same axis the tone curves use, so a curve
    /// drawn here lines up with one drawn there. **x does not wrap**: black and
    /// white are not the same place, and joining them would be a seam invented
    /// rather than removed.
    ///
    /// Desaturating the shadows is the standard move -- noise in the blacks is
    /// chroma noise, and it is cheaper to take the colour out of it than to
    /// denoise it.
    ToneCurve againstLuma;

    /// Hue against hue: where a hue is moved *to*. Wraps like `againstHue`, and
    /// neutral in the middle like the others, but the value is an angle rather
    /// than a multiplier -- 0.5 leaves a hue alone, and the ends move it a
    /// sixth of a turn each way.
    ///
    /// A sixth, because that reaches the neighbouring primary and no further.
    /// This is for correcting a hue that came out wrong -- a sky that went
    /// cyan, foliage that went yellow -- not for recolouring, and a control
    /// that could swing red to green would make the useful range a sliver at
    /// its centre.
    ToneCurve hueShift;

    [[nodiscard]] bool isIdentity() const {
        return againstHue.isIdentity() && againstLuma.isIdentity() && hueShift.isIdentity();
    }

    /// How far the ends of `hueShift` move a hue, as a fraction of the circle.
    static constexpr double kHueShiftRange = 1.0 / 6.0;

    friend bool operator==(const ColorCurves&, const ColorCurves&) = default;
};

/// The four curves a primary grade has: one on luma and one per channel.
struct ToneCurves {
    ToneCurve master;
    ToneCurve red;
    ToneCurve green;
    ToneCurve blue;

    [[nodiscard]] bool isIdentity() const {
        return master.isIdentity() && red.isIdentity() && green.isIdentity() && blue.isIdentity();
    }

    friend bool operator==(const ToneCurves&, const ToneCurves&) = default;
};

}  // namespace zaro::model
