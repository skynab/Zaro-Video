#pragma once

#include <array>
#include <cstdint>
#include <map>

#include "zaro/core/model/ToneCurve.h"
#include "zaro/core/render/CurveTable.h"

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
class ColorCurveTable {
public:
    /// 256 entries over the circle: about a degree and a half each, which is
    /// finer than the eye resolves hue and finer than the 8-bit chroma most
    /// footage arrives with can distinguish.
    static constexpr std::int32_t kEntries = 256;

    ColorCurveTable() = default;
    explicit ColorCurveTable(const model::ColorCurves& curves);

    /// True when the curve does nothing, so the whole step can be skipped.
    [[nodiscard]] bool isIdentity() const noexcept { return identity_; }

    /// The saturation multiplier at a hue, which is 0 to 1 around the circle.
    ///
    /// Wraps rather than clamps, and interpolates between entries: a table
    /// sampled without interpolation puts 256 visible steps around a gradient.
    [[nodiscard]] float saturationAt(float hue) const;

    /// The saturation multiplier at a brightness, indexed exactly as the tone
    /// curves are -- `CurveTable::indexFor`, which folds all of [0, inf) onto
    /// [0, 1]. Sharing that index is what makes a curve drawn against luma line
    /// up with one drawn against the tones.
    ///
    /// Clamps rather than wraps: black and white are not the same place.
    [[nodiscard]] float saturationAtLuma(float linear) const;

    /// Where a hue is moved to, as a turn around the circle. Already wrapped
    /// into [0, 1), so it can be used as a hue directly.
    ///
    /// Takes the hue rather than returning an offset, because the shift is
    /// looked up by the hue it applies to and adding it is the caller's only
    /// use for it -- returning the offset would make every call site repeat the
    /// same wrap.
    [[nodiscard]] float shiftedHue(float hue) const;

    /// How far this hue moves, in turns, before the wrap.
    ///
    /// The offset rather than the destination, because this is what can be
    /// *interpolated*. A destination wraps, and a linear blend between 0.98 and
    /// 0.03 is 0.5 -- a hue on the far side of the circle from either. The
    /// offset is continuous across the seam, so sampling it and wrapping
    /// afterwards is the only order that gives the same answer everywhere.
    [[nodiscard]] float hueOffsetAt(float hue) const;

    /// Whether each curve does anything.
    [[nodiscard]] bool hasHue() const noexcept { return !hueIdentity_; }
    [[nodiscard]] bool hasLuma() const noexcept { return !lumaIdentity_; }
    [[nodiscard]] bool hasShift() const noexcept { return !shiftIdentity_; }

    /// The entries themselves, for upload as a texture.
    [[nodiscard]] const float* data() const noexcept { return entries_.data(); }
    [[nodiscard]] std::size_t byteSize() const noexcept { return entries_.size() * sizeof(float); }

private:
    bool identity_{true};
    bool hueIdentity_{true};
    bool lumaIdentity_{true};
    bool shiftIdentity_{true};
    /// The multiplier at each hue, already doubled out of the curve's 0.5
    /// neutral.
    std::array<float, kEntries> entries_{};
    /// The same against brightness. Its own array rather than a second channel
    /// of the first, because the two are indexed differently and interleaving
    /// them would make every read compute both indices.
    std::array<float, kEntries> luma_{};
    /// The offset, in turns, at each hue. An offset rather than a destination
    /// so the identity row is zeros and a table nobody set costs nothing to
    /// read past.
    std::array<float, kEntries> shift_{};
};

/// One baked hue table per clip, rebuilt only when that clip's curve changes.
///
/// The same reasoning as `CurveTableCache`: a grade is dragged for seconds and
/// changes on approximately no frames at all, so rebuilding 256 entries per
/// frame would be work nobody asked for. Comparing the curve is cheaper than
/// trusting a dirty flag somebody has to remember to set.
class ColorCurveTableCache {
public:
    [[nodiscard]] const ColorCurveTable& tableFor(std::uint64_t key,
                                                  const model::ColorCurves& curves);

    [[nodiscard]] std::size_t size() const noexcept { return cached_.size(); }
    [[nodiscard]] std::int64_t builds() const noexcept { return builds_; }
    void clear() { cached_.clear(); }

private:
    struct Entry {
        bool built{false};
        model::ColorCurves curves;
        ColorCurveTable table;
    };
    std::map<std::uint64_t, Entry> cached_;
    std::int64_t builds_{0};
};

}  // namespace zaro::render
