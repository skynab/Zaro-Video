#include "zaro/core/model/ToneCurve.h"

#include <algorithm>
#include <cmath>

namespace zaro::model {
namespace {

/// Fritsch–Carlson slope limiting.
///
/// The secant slopes say which way each segment goes; the tangents are then
/// clipped so no segment can turn back on itself. This is the whole reason for
/// choosing this spline over a natural one: a natural cubic through a steep
/// step overshoots, and an overshooting tone curve puts a dark halo above a
/// highlight.
std::vector<double> monotonicTangents(const std::vector<CurvePoint>& points) {
    const std::size_t count = points.size();
    std::vector<double> secants(count > 0 ? count - 1 : 0, 0.0);
    for (std::size_t i = 0; i + 1 < count; ++i) {
        const double run = points[i + 1].x - points[i].x;
        secants[i] = run > 0.0 ? (points[i + 1].y - points[i].y) / run : 0.0;
    }

    std::vector<double> tangents(count, 0.0);
    if (count < 2) {
        return tangents;
    }
    tangents.front() = secants.front();
    tangents.back() = secants.back();
    for (std::size_t i = 1; i + 1 < count; ++i) {
        // A local extremum gets a flat tangent: that is what stops the curve
        // continuing past a point it was meant to turn at.
        tangents[i] =
            secants[i - 1] * secants[i] <= 0.0 ? 0.0 : (secants[i - 1] + secants[i]) / 2.0;
    }

    for (std::size_t i = 0; i + 1 < count; ++i) {
        if (secants[i] == 0.0) {
            tangents[i] = 0.0;
            tangents[i + 1] = 0.0;
            continue;
        }
        const double alpha = tangents[i] / secants[i];
        const double beta = tangents[i + 1] / secants[i];
        const double magnitude = std::hypot(alpha, beta);
        if (magnitude > 3.0) {
            const double scale = 3.0 / magnitude;
            tangents[i] = scale * alpha * secants[i];
            tangents[i + 1] = scale * beta * secants[i];
        }
    }
    return tangents;
}

}  // namespace

bool ToneCurve::isIdentity() const {
    if (points_.size() < 2) {
        return true;
    }
    // Two points on the diagonal describe the identity as surely as no points
    // do, and a UI that starts a curve with its endpoints in place should not
    // make every clip pay for a table.
    return std::all_of(points_.begin(), points_.end(),
                       [](const CurvePoint& point) { return point.x == point.y; });
}

void ToneCurve::set(const CurvePoint& point) {
    const auto at = std::lower_bound(
        points_.begin(), points_.end(), point,
        [](const CurvePoint& lhs, const CurvePoint& rhs) { return lhs.x < rhs.x; });
    if (at != points_.end() && at->x == point.x) {
        at->y = point.y;
        return;
    }
    points_.insert(at, point);
}

bool ToneCurve::removeAt(double x, double tolerance) {
    const auto at = std::find_if(points_.begin(), points_.end(), [&](const CurvePoint& point) {
        return std::fabs(point.x - x) <= tolerance;
    });
    if (at == points_.end()) {
        return false;
    }
    points_.erase(at);
    return true;
}

double ToneCurve::valueAt(double x) const {
    if (points_.size() < 2) {
        return x;
    }
    // Held outside the control points, never extrapolated. A tone curve's
    // tangent at its last point says nothing about what is beyond it, and
    // following it produces values far outside the range being mapped.
    if (x <= points_.front().x) {
        return points_.front().y;
    }
    if (x >= points_.back().x) {
        return points_.back().y;
    }

    const auto after =
        std::lower_bound(points_.begin(), points_.end(), x,
                         [](const CurvePoint& point, double query) { return point.x < query; });
    const std::size_t index = static_cast<std::size_t>(after - points_.begin()) - 1;
    const CurvePoint& from = points_[index];
    const CurvePoint& to = points_[index + 1];

    const double run = to.x - from.x;
    if (run <= 0.0) {
        return to.y;
    }
    const std::vector<double> tangents = monotonicTangents(points_);
    const double t = (x - from.x) / run;
    const double t2 = t * t;
    const double t3 = t2 * t;

    // Hermite basis.
    const double h00 = (2.0 * t3) - (3.0 * t2) + 1.0;
    const double h10 = t3 - (2.0 * t2) + t;
    const double h01 = (-2.0 * t3) + (3.0 * t2);
    const double h11 = t3 - t2;

    return (h00 * from.y) + (h10 * run * tangents[index]) + (h01 * to.y) +
           (h11 * run * tangents[index + 1]);
}

}  // namespace zaro::model
