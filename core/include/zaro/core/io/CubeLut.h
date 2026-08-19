#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "zaro/core/Error.h"

namespace zaro::io {

/// A LUT read from a .cube file.
///
/// Both shapes the format allows: a 1D LUT of `size` entries per channel, and a
/// 3D LUT of `size`³ entries. They are kept as read — domain, size and all —
/// rather than immediately resampled, so that whatever is done with them later
/// is done once and knowingly.
class CubeLut {
public:
    enum class Shape : std::uint8_t { OneD, ThreeD };

    [[nodiscard]] Shape shape() const noexcept { return shape_; }
    [[nodiscard]] std::int32_t size() const noexcept { return size_; }
    [[nodiscard]] const std::string& title() const noexcept { return title_; }

    /// The input range the table covers. Almost always 0..1, but the format
    /// allows otherwise and a log LUT that ignored it would be reading the
    /// wrong part of its own table.
    [[nodiscard]] const std::array<float, 3>& domainMin() const noexcept { return domainMin_; }
    [[nodiscard]] const std::array<float, 3>& domainMax() const noexcept { return domainMax_; }

    /// Look up a colour, interpolating. Inputs outside the domain are clamped
    /// to it: a LUT says nothing about values it was not built for, and
    /// extrapolating a look produces colours nobody chose.
    void apply(float& r, float& g, float& b) const;

    [[nodiscard]] static Result<CubeLut> parse(const std::string& text);
    [[nodiscard]] static Result<CubeLut> load(const std::string& path);

private:
    [[nodiscard]] std::array<float, 3> entryAt(std::int32_t index) const;

    Shape shape_{Shape::ThreeD};
    std::int32_t size_{0};
    std::string title_;
    std::array<float, 3> domainMin_{0.0F, 0.0F, 0.0F};
    std::array<float, 3> domainMax_{1.0F, 1.0F, 1.0F};
    /// RGB triples. For a 3D LUT the red index moves fastest, which is what the
    /// format specifies and the single most common way to get a .cube reader
    /// wrong -- the result is a picture with its red and blue axes swapped,
    /// which looks like a colour problem rather than an indexing one.
    std::vector<float> entries_;
};

}  // namespace zaro::io
