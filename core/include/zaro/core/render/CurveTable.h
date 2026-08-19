#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <map>

#include "zaro/core/media/VideoFrame.h"
#include "zaro/core/model/ToneCurve.h"

namespace zaro::render {

/// Tone curves baked into a linear-in, linear-out lookup table.
///
/// The curves are *defined* in the display-encoded domain — that is what a
/// curve's axes mean to whoever draws one, and what the scopes read
/// ([ADR-010](docs/adr/0010-scopes-measure-the-display-signal.md)). The
/// compositor works in linear light. Rather than encode and decode around every
/// pixel, the whole round trip is baked once into a table: encode, apply the
/// curve, decode.
///
/// The table is indexed by `sqrt(v / (1 + v))`, which maps all of [0, ∞) onto
/// [0, 1]. Three arithmetic operations, identical on the CPU and in the shader,
/// with no transfer function needed on either side of the sampling. It also
/// spends its resolution where the eye does: a thousandth of a stop of light
/// lands 32 entries in, while everything above nine times white shares the last
/// fifty. A linear index would give the shadows almost no entries at all.
class CurveTable {
public:
    static constexpr std::int32_t kEntries = 1024;

    /// The index for a linear value. Kept as a function rather than written out
    /// at each use, because the shader has to compute exactly the same thing
    /// and there must be one statement of what "exactly" is.
    [[nodiscard]] static float indexFor(float linear) {
        const float clamped = linear > 0.0F ? linear : 0.0F;
        return std::sqrt(clamped / (1.0F + clamped));
    }

    /// The inverse, used to build the table.
    [[nodiscard]] static float linearFor(float index) {
        const float u2 = index * index;
        return u2 >= 1.0F ? 1e6F : u2 / (1.0F - u2);
    }

    CurveTable() = default;
    CurveTable(const model::ToneCurves& curves, media::TransferFunction transfer);

    [[nodiscard]] bool isIdentity() const noexcept { return identity_; }

    /// Interleaved RGB, `kEntries` triples, for upload as a texture.
    [[nodiscard]] const float* data() const noexcept { return entries_.data(); }
    [[nodiscard]] std::size_t byteSize() const noexcept { return entries_.size() * sizeof(float); }

    /// Look one channel up, interpolating between entries. `channel` is 0, 1
    /// or 2.
    [[nodiscard]] float apply(float linear, std::int32_t channel) const;

private:
    bool identity_{true};
    /// RGB triples rather than RGBA: nothing here has an alpha, and the fourth
    /// component would be a quarter of the upload spent on padding.
    std::array<float, static_cast<std::size_t>(kEntries) * 3> entries_{};
};

/// Tables kept for as long as their curves are unchanged.
///
/// Building one costs three thousand spline evaluations and as many `pow`s.
/// That is nothing once and ruinous per frame, and a grade that is not being
/// edited changes on approximately no frames at all. Keyed by clip, and
/// rebuilt only when that clip's curves are no longer the ones the table was
/// built from — comparing the curves is cheaper than trusting a dirty flag
/// somebody has to remember to set.
class CurveTableCache {
public:
    /// The table for these curves, building it if the cached one is stale.
    /// The reference is valid until the next call for the same key.
    [[nodiscard]] const CurveTable& tableFor(std::uint64_t key, const model::ToneCurves& curves,
                                             media::TransferFunction transfer);

    [[nodiscard]] std::size_t size() const noexcept { return cached_.size(); }
    /// How many tables have actually been built, so a test can show that a
    /// steady grade is not rebuilding one every frame.
    [[nodiscard]] std::int64_t builds() const noexcept { return builds_; }
    void clear() { cached_.clear(); }

private:
    struct Entry {
        bool built{false};
        model::ToneCurves curves;
        media::TransferFunction transfer{};
        CurveTable table;
    };
    std::map<std::uint64_t, Entry> cached_;
    std::int64_t builds_{0};
};

}  // namespace zaro::render
