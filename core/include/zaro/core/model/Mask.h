#pragma once

#include <cstdint>

#include "zaro/core/model/MaskPath.h"

namespace zaro::model {

/// A shape that limits where a clip is visible.
///
/// In **output** coordinates, not the clip's: a mask is where on the screen the
/// clip shows through, and it stays put when the clip moves. That is what makes
/// it usable for a split screen or a window into a plate — a mask that travelled
/// with its clip would be a crop, which is a different tool.
enum class MaskShape : std::uint8_t {
    None,
    Rectangle,
    Ellipse,
    /// An arbitrary closed path of cubic segments. `Mask::path` holds it, and
    /// `width`, `height`, `centreX` and `centreY` are ignored -- a path carries
    /// its own position, and a box around it would be a second place for that
    /// to be recorded.
    Path,
};

[[nodiscard]] const char* toString(MaskShape shape) noexcept;
[[nodiscard]] MaskShape maskShapeFromString(const char* name) noexcept;

struct Mask {
    MaskShape shape{MaskShape::None};

    double width{600.0};
    double height{400.0};
    double centreX{0.0};
    double centreY{0.0};
    double cornerRadius{0.0};
    double feather{0.0};

    /// Show everything *outside* the shape instead. A vignette and a spotlight
    /// are the same mask with this flipped, and having to draw the complement
    /// by hand is how people end up with two masks that must be kept agreeing.
    bool inverted{false};

    /// The outline, when `shape` is `Path`. Empty otherwise.
    MaskPath path;

    [[nodiscard]] bool isSet() const noexcept { return shape != MaskShape::None; }

    friend bool operator==(const Mask&, const Mask&) = default;
};

}  // namespace zaro::model
