#include "zaro/core/render/CurveTable.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "zaro/core/render/ColorPipeline.h"

namespace zaro::render {

CurveTable::CurveTable(const model::ToneCurves& curves, media::TransferFunction transfer) {
    identity_ = curves.isIdentity();
    if (identity_) {
        return;
    }

    const model::ToneCurve* perChannel[3] = {&curves.red, &curves.green, &curves.blue};
    for (std::int32_t i = 0; i < kEntries; ++i) {
        const float index = static_cast<float>(i) / static_cast<float>(kEntries - 1);
        const float linear = linearFor(index);
        const float encoded = fromLinearScalar(linear, transfer);

        for (std::int32_t channel = 0; channel < 3; ++channel) {
            // The channel curve first, then the master. A master curve is a
            // statement about the picture's tones, and the picture is what the
            // per-channel curves have already made it.
            const double shaped = perChannel[channel]->valueAt(static_cast<double>(encoded));
            const double mastered = curves.master.valueAt(shaped);
            entries_[(static_cast<std::size_t>(i) * 3) + static_cast<std::size_t>(channel)] =
                toLinearScalar(static_cast<float>(mastered), transfer);
        }
    }
}

float CurveTable::apply(float linear, std::int32_t channel) const {
    if (identity_ || channel < 0 || channel > 2) {
        return linear;
    }
    const float index = indexFor(linear) * static_cast<float>(kEntries - 1);
    const auto low = static_cast<std::int32_t>(index);
    const std::int32_t high = std::min(low + 1, kEntries - 1);
    const float fraction = index - static_cast<float>(low);

    const float a =
        entries_[(static_cast<std::size_t>(low) * 3) + static_cast<std::size_t>(channel)];
    const float b =
        entries_[(static_cast<std::size_t>(high) * 3) + static_cast<std::size_t>(channel)];
    return a + ((b - a) * fraction);
}

const CurveTable& CurveTableCache::tableFor(std::uint64_t key, const model::ToneCurves& curves,
                                            media::TransferFunction transfer) {
    Entry& entry = cached_[key];
    if (entry.built && entry.curves == curves && entry.transfer == transfer) {
        return entry.table;
    }
    entry.curves = curves;
    entry.transfer = transfer;
    entry.table = CurveTable{curves, transfer};
    entry.built = true;
    ++builds_;
    return entry.table;
}

}  // namespace zaro::render
