#include "zaro/core/model/Animation.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "zaro/core/Check.h"

namespace zaro::model {
namespace {

/// A cubic bezier's x at parameter t, with the ends pinned to 0 and 1.
double bezierX(double t, double x1, double x2) {
    const double mt = 1.0 - t;
    return (3.0 * mt * mt * t * x1) + (3.0 * mt * t * t * x2) + (t * t * t);
}

double bezierDx(double t, double x1, double x2) {
    const double mt = 1.0 - t;
    return (3.0 * mt * mt * x1) + (6.0 * mt * t * (x2 - x1)) + (3.0 * t * t * (1.0 - x2));
}

double bezierY(double t, double y0, double y1, double y2, double y3) {
    const double mt = 1.0 - t;
    return (mt * mt * mt * y0) + (3.0 * mt * mt * t * y1) + (3.0 * mt * t * t * y2) +
           (t * t * t * y3);
}

/// The bezier parameter whose x is `u`.
///
/// The curve is drawn in (time, value), so reading a value at a time means
/// inverting x first. Newton converges in a handful of steps for the handles
/// people actually draw, but it can walk off a curve with a near-flat region,
/// so a bisection fallback guarantees an answer. Iteration counts are fixed:
/// this runs per parameter per frame, and a loop that usually exits early but
/// occasionally does not is exactly the kind of thing that shows up as a
/// dropped frame.
double solveBezierT(double u, double x1, double x2) {
    constexpr double kEpsilon = 1e-7;

    double t = u;
    for (int i = 0; i < 8; ++i) {
        const double error = bezierX(t, x1, x2) - u;
        if (std::fabs(error) < kEpsilon) {
            return t;
        }
        const double slope = bezierDx(t, x1, x2);
        if (std::fabs(slope) < kEpsilon) {
            break;  // flat here; Newton has nothing to follow
        }
        t -= error / slope;
        if (t < 0.0 || t > 1.0) {
            break;  // left the segment; bisection will bring it back
        }
    }

    double low = 0.0;
    double high = 1.0;
    t = u;
    for (int i = 0; i < 40; ++i) {
        const double x = bezierX(t, x1, x2);
        if (std::fabs(x - u) < kEpsilon) {
            break;
        }
        if (x < u) {
            low = t;
        } else {
            high = t;
        }
        t = (low + high) * 0.5;
    }
    return t;
}

/// Handle x offsets, made safe to invert.
///
/// If the two handles reach past each other the curve doubles back, and a
/// doubled-back curve has two values at one time — which a parameter cannot
/// have. Scaling both down until they meet is the standard fix and preserves
/// the ratio between them, so the curve keeps the shape that was drawn as
/// nearly as a function can.
void clampHandles(double& x1, double& x2) {
    x1 = std::clamp(x1, 0.0, 1.0);
    x2 = std::clamp(x2, 0.0, 1.0);
    const double reach = x1 + x2;
    if (reach > 1.0) {
        x1 /= reach;
        x2 /= reach;
    }
}

double evaluateSegment(const Keyframe& from, const Keyframe& to, double u) {
    switch (from.interpolation) {
        case Interpolation::Hold:
            return from.value;
        case Interpolation::Linear:
            return from.value + ((to.value - from.value) * u);
        case Interpolation::Bezier:
            break;
    }

    double x1 = from.out.dx;
    double x2 = to.in.dx;
    clampHandles(x1, x2);
    const double t = solveBezierT(u, x1, 1.0 - x2);
    return bezierY(t, from.value, from.value + from.out.dy, to.value - to.in.dy, to.value);
}

}  // namespace

const char* toString(Interpolation interpolation) noexcept {
    switch (interpolation) {
        case Interpolation::Hold:
            return "hold";
        case Interpolation::Linear:
            return "linear";
        case Interpolation::Bezier:
            return "bezier";
    }
    return "linear";
}

Interpolation interpolationFromString(const char* name) noexcept {
    if (name == nullptr) {
        return Interpolation::Linear;
    }
    if (std::strcmp(name, "hold") == 0) {
        return Interpolation::Hold;
    }
    if (std::strcmp(name, "bezier") == 0) {
        return Interpolation::Bezier;
    }
    return Interpolation::Linear;
}

void Curve::set(const Keyframe& keyframe) {
    const auto at = std::lower_bound(
        keys_.begin(), keys_.end(), keyframe,
        [](const Keyframe& lhs, const Keyframe& rhs) { return lhs.time < rhs.time; });
    if (at != keys_.end() && at->time == keyframe.time) {
        *at = keyframe;
        return;
    }
    keys_.insert(at, keyframe);
}

bool Curve::removeAt(const time::RationalTime& time) {
    const auto at = std::find_if(keys_.begin(), keys_.end(),
                                 [&](const Keyframe& key) { return key.time == time; });
    if (at == keys_.end()) {
        return false;
    }
    keys_.erase(at);
    return true;
}

const Keyframe* Curve::at(const time::RationalTime& time) const {
    const auto found = std::find_if(keys_.begin(), keys_.end(),
                                    [&](const Keyframe& key) { return key.time == time; });
    return found == keys_.end() ? nullptr : &*found;
}

double Curve::valueAtSeconds(double seconds) const {
    ZARO_CHECK(!keys_.empty(), "valueAtSeconds on a curve with no keyframes");

    if (keys_.size() == 1) {
        return keys_.front().value;
    }
    if (seconds <= keys_.front().time.toSecondsDouble()) {
        return keys_.front().value;
    }
    if (seconds >= keys_.back().time.toSecondsDouble()) {
        return keys_.back().value;
    }

    // The first keyframe at or after the query, so the segment is the pair
    // straddling it. Bisection rather than a scan: a long automation curve can
    // hold thousands of keyframes and this runs per frame.
    const auto after = std::lower_bound(
        keys_.begin(), keys_.end(), seconds,
        [](const Keyframe& key, double query) { return key.time.toSecondsDouble() < query; });
    if (after == keys_.begin()) {
        return keys_.front().value;
    }
    const Keyframe& to = *after;
    const Keyframe& from = *(after - 1);

    const double fromSeconds = from.time.toSecondsDouble();
    const double span = to.time.toSecondsDouble() - fromSeconds;
    if (span <= 0.0) {
        return to.value;  // cannot happen while set() dedupes, but do not divide by it
    }
    return evaluateSegment(from, to, (seconds - fromSeconds) / span);
}

std::span<const Param> allParams() noexcept {
    static constexpr Param kAll[] = {
        Param::PositionX,  Param::PositionY,     Param::ScaleX,
        Param::ScaleY,     Param::Opacity,       Param::RotationDegrees,
        Param::AnchorX,    Param::AnchorY,       Param::GainDb,
        Param::Pan,        Param::Temperature,   Param::Tint,
        Param::Exposure,   Param::Contrast,      Param::Saturation,
        Param::MaskX,      Param::MaskY,         Param::StabiliseX,
        Param::StabiliseY, Param::StabiliseZoom, Param::TextReveal,
        Param::TimeRemap};
    return kAll;
}

const char* toString(Param param) noexcept {
    switch (param) {
        case Param::PositionX:
            return "positionX";
        case Param::PositionY:
            return "positionY";
        case Param::ScaleX:
            return "scaleX";
        case Param::ScaleY:
            return "scaleY";
        case Param::RotationDegrees:
            return "rotationDegrees";
        case Param::AnchorX:
            return "anchorX";
        case Param::AnchorY:
            return "anchorY";
        case Param::Opacity:
            return "opacity";
        case Param::GainDb:
            return "gainDb";
        case Param::Pan:
            return "pan";
        case Param::Temperature:
            return "temperature";
        case Param::Tint:
            return "tint";
        case Param::Exposure:
            return "exposure";
        case Param::Contrast:
            return "contrast";
        case Param::Saturation:
            return "saturation";
        case Param::MaskX:
            return "maskX";
        case Param::MaskY:
            return "maskY";
        case Param::StabiliseX:
            return "stabiliseX";
        case Param::StabiliseY:
            return "stabiliseY";
        case Param::StabiliseZoom:
            return "stabiliseZoom";
        case Param::TextReveal:
            return "textReveal";
        case Param::TimeRemap:
            return "timeRemap";
    }
    return "";
}

bool paramFromString(const char* name, Param& out) noexcept {
    if (name == nullptr) {
        return false;
    }
    for (Param candidate : allParams()) {
        if (std::strcmp(name, toString(candidate)) == 0) {
            out = candidate;
            return true;
        }
    }
    return false;
}

const Curve* ClipAnimation::find(Param param) const {
    const auto found = curves_.find(param);
    return found == curves_.end() ? nullptr : &found->second;
}

void ClipAnimation::pruneEmpty() {
    for (auto it = curves_.begin(); it != curves_.end();) {
        it = it->second.empty() ? curves_.erase(it) : std::next(it);
    }
}

}  // namespace zaro::model
