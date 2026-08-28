#include "zaro/core/render/LutTable.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "zaro/core/render/ColorPipeline.h"

namespace zaro::render {

LutTable::LutTable(const io::CubeLut& lut, media::TransferFunction transfer) {
    entries_.assign(static_cast<std::size_t>(kSize) * kSize * kSize * 3, 0.0F);

    // The axis stops where the LUT's own domain does, so the last grid point is
    // exactly the brightest light the LUT has anything to say about.
    const float domainTop = std::max({lut.domainMax()[0], lut.domainMax()[1], lut.domainMax()[2]});
    axisMax_ = CurveTable::indexFor(toLinearScalar(domainTop, transfer));

    for (std::int32_t bi = 0; bi < kSize; ++bi) {
        for (std::int32_t gi = 0; gi < kSize; ++gi) {
            for (std::int32_t ri = 0; ri < kSize; ++ri) {
                const auto axis = [this](std::int32_t index) {
                    return CurveTable::linearFor(axisMax_ * static_cast<float>(index) /
                                                 static_cast<float>(kSize - 1));
                };
                // Linear in, encoded for the LUT, then back to linear. The
                // round trip lives here so that nothing downstream needs a
                // transfer function.
                float r = fromLinearScalar(axis(ri), transfer);
                float g = fromLinearScalar(axis(gi), transfer);
                float b = fromLinearScalar(axis(bi), transfer);
                lut.apply(r, g, b);

                const std::size_t at =
                    (static_cast<std::size_t>(ri) + (static_cast<std::size_t>(gi) * kSize) +
                     (static_cast<std::size_t>(bi) * kSize * kSize)) *
                    3;
                entries_[at] = CurveTable::indexFor(toLinearScalar(r, transfer));
                entries_[at + 1] = CurveTable::indexFor(toLinearScalar(g, transfer));
                entries_[at + 2] = CurveTable::indexFor(toLinearScalar(b, transfer));
            }
        }
    }
}

void LutTable::apply(float& r, float& g, float& b, float amount) const {
    if (entries_.empty() || amount <= 0.0F) {
        return;
    }

    const float last = static_cast<float>(kSize - 1);
    // Scaled into the cube's own axis, and clamped to it: above the LUT's
    // domain the answer is the last entry, which is what the LUT says.
    const float scale = axisMax_ > 0.0001F ? last / axisMax_ : last;
    const float fr = std::min(CurveTable::indexFor(r) * scale, last);
    const float fg = std::min(CurveTable::indexFor(g) * scale, last);
    const float fb = std::min(CurveTable::indexFor(b) * scale, last);

    const auto low = [](float value) {
        return std::clamp(static_cast<std::int32_t>(value), 0, kSize - 1);
    };
    const std::int32_t r0 = low(fr);
    const std::int32_t g0 = low(fg);
    const std::int32_t b0 = low(fb);
    const std::int32_t r1 = std::min(r0 + 1, kSize - 1);
    const std::int32_t g1 = std::min(g0 + 1, kSize - 1);
    const std::int32_t b1 = std::min(b0 + 1, kSize - 1);
    const float dr = std::clamp(fr - static_cast<float>(r0), 0.0F, 1.0F);
    const float dg = std::clamp(fg - static_cast<float>(g0), 0.0F, 1.0F);
    const float db = std::clamp(fb - static_cast<float>(b0), 0.0F, 1.0F);

    const auto at = [this](std::int32_t ri, std::int32_t gi, std::int32_t bi, std::size_t channel) {
        const std::size_t index =
            (static_cast<std::size_t>(ri) + (static_cast<std::size_t>(gi) * kSize) +
             (static_cast<std::size_t>(bi) * kSize * kSize)) *
                3 +
            channel;
        return entries_[index];
    };

    float out[3] = {r, g, b};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const float c00 =
            at(r0, g0, b0, channel) + ((at(r1, g0, b0, channel) - at(r0, g0, b0, channel)) * dr);
        const float c10 =
            at(r0, g1, b0, channel) + ((at(r1, g1, b0, channel) - at(r0, g1, b0, channel)) * dr);
        const float c01 =
            at(r0, g0, b1, channel) + ((at(r1, g0, b1, channel) - at(r0, g0, b1, channel)) * dr);
        const float c11 =
            at(r0, g1, b1, channel) + ((at(r1, g1, b1, channel) - at(r0, g1, b1, channel)) * dr);
        const float c0 = c00 + ((c10 - c00) * dg);
        const float c1 = c01 + ((c11 - c01) * dg);
        out[channel] = c0 + ((c1 - c0) * db);
    }

    // Blended rather than switched: the amount control is how a look is dialled
    // back, and every grading tool has one because a look at full strength is
    // rarely the one anybody ships. Blended after un-warping, so half a look is
    // half of it in the light rather than half in an index space.
    const float blend = std::clamp(amount, 0.0F, 1.0F);
    r += (CurveTable::linearFor(out[0]) - r) * blend;
    g += (CurveTable::linearFor(out[1]) - g) * blend;
    b += (CurveTable::linearFor(out[2]) - b) * blend;
}

const LutTable* LutCache::tableFor(const std::string& path, media::TransferFunction transfer) {
    if (path.empty()) {
        return nullptr;
    }
    auto found = cached_.find(path);
    if (found != cached_.end() && found->second.transfer == transfer) {
        return found->second.table.isValid() ? &found->second.table : nullptr;
    }

    Entry entry;
    entry.transfer = transfer;
    auto loaded = io::CubeLut::load(path);
    if (!loaded) {
        // Remembered as a failure rather than retried every frame: a missing
        // LUT would otherwise be a file-system call per clip per frame.
        entry.error = loaded.error().toString();
    } else {
        entry.table = LutTable{*loaded, transfer};
    }
    ++loads_;
    cached_[path] = std::move(entry);
    const Entry& stored = cached_[path];
    return stored.table.isValid() ? &stored.table : nullptr;
}

const std::string& LutCache::errorFor(const std::string& path) const {
    static const std::string none;
    const auto found = cached_.find(path);
    return found == cached_.end() ? none : found->second.error;
}

}  // namespace zaro::render
