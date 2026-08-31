#pragma once

#include <array>
#include <cstdint>
#include <map>

#include "zaro/core/model/ToneCurve.h"

namespace zaro::render {

/// A hue curve baked into a wrapped lookup table.
///
/// Baked for the same reason the tone curves are: the curve is evaluated once
/// per entry rather than once per pixel, the shader samples the identical
/// table, and neither side has to know anything about the other's arithmetic.
///
/// **The wrap happens here, once.** Hue is a circle and `model::ToneCurve` is
/// not: evaluated directly it would run flat from the last point to x = 1 and
/// flat from x = 0 to the first, putting a seam at red -- which is the single
/// most common hue anybody reaches for. Baking evaluates an extended copy of
/// the curve with its points repeated a turn either side, so the interpolation
/// carries across the seam exactly as it does anywhere else. Doing that per
/// pixel would mean rebuilding a point list six million times a frame.
class HueTable {
public:
    /// 256 entries over the circle: about a degree and a half each, which is
    /// finer than the eye resolves hue and finer than the 8-bit chroma most
    /// footage arrives with can distinguish.
    static constexpr std::int32_t kEntries = 256;

    HueTable() = default;
    explicit HueTable(const model::HueCurves& curves);

    /// True when the curve does nothing, so the whole step can be skipped.
    [[nodiscard]] bool isIdentity() const noexcept { return identity_; }

    /// The saturation multiplier at a hue, which is 0 to 1 around the circle.
    ///
    /// Wraps rather than clamps, and interpolates between entries: a table
    /// sampled without interpolation puts 256 visible steps around a gradient.
    [[nodiscard]] float saturationAt(float hue) const;

    /// The entries themselves, for upload as a texture.
    [[nodiscard]] const float* data() const noexcept { return entries_.data(); }
    [[nodiscard]] std::size_t byteSize() const noexcept { return entries_.size() * sizeof(float); }

private:
    bool identity_{true};
    /// The multiplier at each hue, already doubled out of the curve's 0.5
    /// neutral.
    std::array<float, kEntries> entries_{};
};

/// One baked hue table per clip, rebuilt only when that clip's curve changes.
///
/// The same reasoning as `CurveTableCache`: a grade is dragged for seconds and
/// changes on approximately no frames at all, so rebuilding 256 entries per
/// frame would be work nobody asked for. Comparing the curve is cheaper than
/// trusting a dirty flag somebody has to remember to set.
class HueTableCache {
public:
    [[nodiscard]] const HueTable& tableFor(std::uint64_t key, const model::HueCurves& curves);

    [[nodiscard]] std::size_t size() const noexcept { return cached_.size(); }
    [[nodiscard]] std::int64_t builds() const noexcept { return builds_; }
    void clear() { cached_.clear(); }

private:
    struct Entry {
        bool built{false};
        model::HueCurves curves;
        HueTable table;
    };
    std::map<std::uint64_t, Entry> cached_;
    std::int64_t builds_{0};
};

}  // namespace zaro::render
