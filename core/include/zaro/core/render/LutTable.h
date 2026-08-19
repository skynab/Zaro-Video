#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "zaro/core/io/CubeLut.h"
#include "zaro/core/media/VideoFrame.h"
#include "zaro/core/render/CurveTable.h"

namespace zaro::render {

/// A .cube LUT baked into a linear-in, linear-out cube.
///
/// The same shape of decision as `CurveTable` ([ADR-012](docs/adr/0012)): the
/// LUT is defined on display-encoded values, the compositor works in linear
/// light, and rather than encode and decode around every pixel the whole round
/// trip is baked once. The shader then samples a cube and knows nothing about
/// transfer functions, domains, or the .cube format.
///
/// Each axis is indexed by `CurveTable::indexFor`, so the same three operations
/// place a colour in this cube as place one in a curve's table, and all of
/// [0, ∞) is covered — a LUT is defined over its domain, but the pictures fed
/// through it are not.
class LutTable {
public:
    /// 32 a side: half a megabyte, and finer than the 17 or 33 that authored
    /// LUTs almost always are, so the baking is not what limits the result.
    static constexpr std::int32_t kSize = 32;

    LutTable() = default;
    LutTable(const io::CubeLut& lut, media::TransferFunction transfer);

    [[nodiscard]] bool isValid() const noexcept { return !entries_.empty(); }

    /// Where the LUT's domain ends, on the warped axis.
    ///
    /// The cube covers exactly what the LUT defines and no more. A cube spread
    /// over all of [0, ∞) would spend grid points on light the LUT clamps
    /// anyway, and — worse — would interpolate across the clamp: the sample
    /// either side of white would mix a real value with a clamped one and pull
    /// white itself below 1, so an identity LUT would dim the picture by a
    /// percent. Anything above this is the last entry, which is what the LUT
    /// says about it.
    [[nodiscard]] float axisMax() const noexcept { return axisMax_; }

    /// The stored values are *warped* by `CurveTable::indexFor`, the same way
    /// the axes are, and un-warped after sampling.
    ///
    /// Storing linear values instead makes the identity fail to round trip: the
    /// axis is convex in linear light, so interpolating between two grid points
    /// under-shoots — measurably, about a percent at white on a 32-point cube,
    /// which is three code values of error on a LUT that was supposed to do
    /// nothing. Warping the output too means an identity LUT stores exactly the
    /// grid coordinate at every point, and linear interpolation of that is
    /// exact.

    /// Interleaved RGB, `kSize`³ triples, red fastest then green then blue.
    [[nodiscard]] const float* data() const noexcept { return entries_.data(); }
    [[nodiscard]] std::size_t floatCount() const noexcept { return entries_.size(); }

    /// Apply, blending by `amount`. Trilinear, matching what a GPU sampler
    /// does with the same cube.
    void apply(float& r, float& g, float& b, float amount) const;

private:
    float axisMax_{1.0F};
    std::vector<float> entries_;
};

/// Baked cubes, kept for as long as their file is the one asked for.
///
/// Reading and baking a LUT is a file read and thirty thousand interpolations.
/// Keyed by path: two clips using the same look share one cube, which is the
/// common case on a graded timeline.
class LutCache {
public:
    /// The baked cube for a path, or nullptr if it cannot be read. A missing
    /// or broken LUT is a clip that grades without it rather than a render
    /// that fails — the same reasoning as an unreadable frame.
    [[nodiscard]] const LutTable* tableFor(const std::string& path,
                                           media::TransferFunction transfer);

    /// Why the last load of this path failed, for a UI that wants to say so.
    [[nodiscard]] const std::string& errorFor(const std::string& path) const;

    [[nodiscard]] std::int64_t loads() const noexcept { return loads_; }
    void clear() { cached_.clear(); }

private:
    struct Entry {
        media::TransferFunction transfer{};
        LutTable table;
        std::string error;
    };
    std::map<std::string, Entry> cached_;
    std::int64_t loads_{0};
};

}  // namespace zaro::render
