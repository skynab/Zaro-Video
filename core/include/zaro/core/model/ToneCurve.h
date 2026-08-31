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

/// Curves whose x axis is hue rather than brightness.
///
/// A different kind of curve from the tone ones below, sharing their machinery
/// and almost none of their meaning:
///
///   * **x wraps.** Hue is a circle, so 0 and 1 are the same place. A curve
///     evaluated without wrapping has a seam at red, which is the single most
///     common hue anybody adjusts. `render::HueTable` is where the wrap
///     happens, once, at bake time.
///   * **0.5 is neutral, not 0.** The value is a multiplier on saturation and
///     the curve has to be able to go both ways, so the middle of the range
///     means "leave it alone": 0 removes the colour, 0.5 keeps it, 1 doubles
///     it. A curve with no points is identity, as the tone curves are, which
///     is not the same as a flat curve at 0.5 -- that is the identity spelled
///     out, and it still costs a table.
struct HueCurves {
    /// Saturation against hue: pull the sky down without touching skin, lift a
    /// tired green, take the ring out of a magenta practical.
    ToneCurve saturation;

    [[nodiscard]] bool isIdentity() const { return saturation.isIdentity(); }

    friend bool operator==(const HueCurves&, const HueCurves&) = default;
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
