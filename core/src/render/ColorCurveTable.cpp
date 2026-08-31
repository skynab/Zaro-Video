#include "zaro/core/render/ColorCurveTable.h"

#include <algorithm>
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

ColorCurveTable::ColorCurveTable(const model::ColorCurves& curves) {
    entries_.fill(1.0F);
    luma_.fill(1.0F);
    shift_.fill(0.0F);
    if (curves.isIdentity()) {
        return;
    }
    identity_ = false;

    // 0.5 is neutral, so the multiplier is twice the curve: 0 removes the
    // colour, 0.5 keeps it, 1 doubles it. Clamped at zero because a negative
    // multiplier would push a pixel through grey and out the other side, which
    // is a different colour rather than a less saturated one.
    const auto multiplier = [](double value) {
        const double scaled = value * 2.0;
        return static_cast<float>(scaled > 0.0 ? scaled : 0.0);
    };

    if (!curves.againstHue.isIdentity()) {
        hueIdentity_ = false;
        const model::ToneCurve curve = wrapped(curves.againstHue);
        for (std::int32_t i = 0; i < kEntries; ++i) {
            const double hue = static_cast<double>(i) / static_cast<double>(kEntries);
            entries_[static_cast<std::size_t>(i)] = multiplier(curve.valueAt(hue));
        }
    }
    if (!curves.hueShift.isIdentity()) {
        shiftIdentity_ = false;
        const model::ToneCurve curve = wrapped(curves.hueShift);
        for (std::int32_t i = 0; i < kEntries; ++i) {
            const double hue = static_cast<double>(i) / static_cast<double>(kEntries);
            // Neutral in the middle like the others, but the value is an angle:
            // 0.5 leaves a hue where it is, and the ends move it
            // `kHueShiftRange` each way.
            const double offset =
                (curve.valueAt(hue) - 0.5) * 2.0 * model::ColorCurves::kHueShiftRange;
            shift_[static_cast<std::size_t>(i)] = static_cast<float>(offset);
        }
    }
    if (!curves.againstLuma.isIdentity()) {
        lumaIdentity_ = false;
        for (std::int32_t i = 0; i < kEntries; ++i) {
            // Indexed as the tone curves are, so a curve drawn here lines up
            // with one drawn there. No wrapping: black and white are not the
            // same place.
            const double at = static_cast<double>(i) / static_cast<double>(kEntries - 1);
            luma_[static_cast<std::size_t>(i)] = multiplier(curves.againstLuma.valueAt(at));
        }
    }
}

float ColorCurveTable::hueOffsetAt(float hue) const {
    if (shiftIdentity_) {
        return 0.0F;
    }
    const float turn = hue - std::floor(hue);
    // Read at the hue the pixel *has*. Reading at the destination instead would
    // make the curve define itself in terms of its own output, which has no
    // fixed point to solve from.
    const float scaled = turn * static_cast<float>(kEntries);
    const auto low = static_cast<std::int32_t>(scaled);
    const float fraction = scaled - static_cast<float>(low);
    const float first = shift_[static_cast<std::size_t>(low % kEntries)];
    const float second = shift_[static_cast<std::size_t>((low + 1) % kEntries)];
    return first + ((second - first) * fraction);
}

float ColorCurveTable::shiftedHue(float hue) const {
    const float turn = hue - std::floor(hue);
    if (shiftIdentity_) {
        return turn;
    }
    const float moved = turn + hueOffsetAt(turn);
    return moved - std::floor(moved);
}

float ColorCurveTable::saturationAtLuma(float linear) const {
    if (lumaIdentity_) {
        return 1.0F;
    }
    const float index = CurveTable::indexFor(linear);
    const float scaled = std::clamp(index, 0.0F, 1.0F) * static_cast<float>(kEntries - 1);
    const auto low = static_cast<std::int32_t>(scaled);
    const std::int32_t high = std::min(low + 1, kEntries - 1);
    const float fraction = scaled - static_cast<float>(low);
    const float first = luma_[static_cast<std::size_t>(low)];
    const float second = luma_[static_cast<std::size_t>(high)];
    return first + ((second - first) * fraction);
}

float ColorCurveTable::saturationAt(float hue) const {
    if (hueIdentity_) {
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

const ColorCurveTable& ColorCurveTableCache::tableFor(std::uint64_t key,
                                                      const model::ColorCurves& curves) {
    Entry& entry = cached_[key];
    if (entry.built && entry.curves == curves) {
        return entry.table;
    }
    entry.curves = curves;
    entry.table = ColorCurveTable{curves};
    entry.built = true;
    ++builds_;
    return entry.table;
}

}  // namespace zaro::render
