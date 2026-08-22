#pragma once

#include <vector>

namespace zaro::model {

/// One point of a mask path, with the handles either side of it.
///
/// Handles are offsets from the point, in the same output pixels the point is
/// in. Stored rather than derived, because a path is edited by dragging them:
/// a curve reconstructed from neighbours would move when a *different* point
/// moved, which is not what dragging one handle means.
///
/// Both handles at zero is a corner. That is the honest representation of a
/// corner rather than a separate flag: a segment leaving a point with no
/// outgoing handle is a straight line to the next one, which is exactly what a
/// cubic with coincident controls degenerates to, so there is one evaluation
/// path rather than two.
struct MaskPoint {
    double x{0.0};
    double y{0.0};
    /// The handle arriving at this point, used by the segment before it.
    double inX{0.0};
    double inY{0.0};
    /// The handle leaving this point, used by the segment after it.
    double outX{0.0};
    double outY{0.0};

    friend bool operator==(const MaskPoint&, const MaskPoint&) = default;
};

/// A closed path of cubic segments, in output coordinates from the centre of
/// the frame -- the same space every other mask lives in.
///
/// Always closed. An open path has no inside, and a mask is a question about
/// what is inside: the alternative would be a stroke, which is a different tool
/// with a different control.
struct MaskPath {
    std::vector<MaskPoint> points;

    /// Fewer than three points cannot enclose anything.
    [[nodiscard]] bool isSet() const noexcept { return points.size() >= 3; }

    friend bool operator==(const MaskPath&, const MaskPath&) = default;
};

}  // namespace zaro::model
