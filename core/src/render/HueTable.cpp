#include "zaro/core/render/HueTable.h"

#include <cmath>

namespace zaro::render {
namespace {

/// The curve with its points repeated a turn either side.
///
/// What makes the seam disappear. A point at hue 0.95 and one at 0.05 are
/// neighbours on the circle and 0.9 apart on the line; without the copies the
/// interpolation between them runs the long way round, flat, through every hue
/// in between.
model::ToneCurve wrapped(const model::ToneCurve& curve) {
    model::ToneCurve out;
    for (const model::CurvePoint& point : curve.points()) {
        out.set(model::CurvePoint{point.x - 1.0, point.y});
        out.set(point);
        out.set(model::CurvePoint{point.x + 1.0, point.y});
    }
    return out;
}

}  // namespace

HueTable::HueTable(const model::HueCurves& curves) {
    if (curves.isIdentity()) {
        entries_.fill(1.0F);
        return;
    }
    identity_ = false;
    const model::ToneCurve curve = wrapped(curves.saturation);
    for (std::int32_t i = 0; i < kEntries; ++i) {
        const double hue = static_cast<double>(i) / static_cast<double>(kEntries);
        // 0.5 is neutral, so the multiplier is twice the curve: 0 removes the
        // colour, 0.5 keeps it, 1 doubles it. Clamped at zero because a
        // negative multiplier would push a pixel through grey and out the other
        // side, which is a different colour rather than a less saturated one.
        const double value = curve.valueAt(hue) * 2.0;
        entries_[static_cast<std::size_t>(i)] = static_cast<float>(value > 0.0 ? value : 0.0);
    }
}

float HueTable::saturationAt(float hue) const {
    if (identity_) {
        return 1.0F;
    }
    // Wrapped into [0, 1) first: a hue computed from floats can land a hair
    // outside its range, and a table read that clamps there would flatten the
    // curve at red without anybody being able to see why.
    float turn = hue - std::floor(hue);
    const float scaled = turn * static_cast<float>(kEntries);
    const auto low = static_cast<std::int32_t>(scaled);
    const float fraction = scaled - static_cast<float>(low);
    const std::int32_t a = low % kEntries;
    const std::int32_t b = (low + 1) % kEntries;
    const float first = entries_[static_cast<std::size_t>(a)];
    const float second = entries_[static_cast<std::size_t>(b)];
    return first + ((second - first) * fraction);
}

const HueTable& HueTableCache::tableFor(std::uint64_t key, const model::HueCurves& curves) {
    Entry& entry = cached_[key];
    if (entry.built && entry.curves == curves) {
        return entry.table;
    }
    entry.curves = curves;
    entry.table = HueTable{curves};
    entry.built = true;
    ++builds_;
    return entry.table;
}

}  // namespace zaro::render
