#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "zaro/core/render/AudioProcessor.h"

namespace zaro::render {

/// Loudness to ITU-R BS.1770 / EBU R128.
///
/// The measurement a delivery specification is written in. A peak meter says
/// whether a mix will clip; this says whether it is as loud as the broadcaster
/// asked for, which is a different question and the one that gets a programme
/// rejected.
///
/// Three figures, all in LUFS:
///
/// - **Momentary** over the last 400 ms, which is what a meter shows moving.
/// - **Short-term** over the last 3 s, which is what a mixer watches.
/// - **Integrated** over everything measured, gated — the number in the spec.
///
/// The gating is not a detail. Without it a programme with quiet passages
/// measures quieter than it sounds, so everyone would push the loud parts up to
/// compensate; the gate is what stopped the loudness war from simply moving.
class LoudnessMeter {
public:
    /// Silence, and anything close enough to it, measures as this rather than
    /// as minus infinity — which no display and no arithmetic handles well.
    static constexpr double kSilence = -70.0;

    LoudnessMeter() = default;
    void configure(double sampleRate, std::int32_t channelCount);
    void reset();

    void feed(const float* const* channels, std::int32_t channelCount, std::int64_t sampleCount);

    [[nodiscard]] double momentaryLufs() const;
    [[nodiscard]] double shortTermLufs() const;
    /// Gated, over everything fed since the last reset.
    [[nodiscard]] double integratedLufs() const;

    /// The largest sample seen, in dBFS. Sample peak, not true peak:
    /// inter-sample peaks need oversampling, and claiming true peak without it
    /// would be a number that passes a check the delivered file fails.
    [[nodiscard]] double samplePeakDbfs() const;

private:
    void pushBlock(double meanSquare);

    static constexpr std::int32_t kMaxChannels = 8;
    /// BS.1770 measures in 400 ms blocks overlapping by three quarters.
    static constexpr std::int32_t kBlocksPerSecond = 10;

    double sampleRate_{48000.0};
    std::int32_t channels_{2};
    std::int64_t blockSamples_{0};
    std::int64_t stepSamples_{0};

    std::array<Biquad, kMaxChannels> shelf_{};
    std::array<Biquad, kMaxChannels> highPass_{};

    /// The samples of the block being filled, already weighted and squared.
    std::vector<double> window_;
    std::int64_t windowFill_{0};
    std::int64_t sinceLastBlock_{0};

    /// Mean square per 400 ms block, in order.
    std::vector<double> blocks_;
    double peak_{0.0};
};

/// Loudness in LUFS from a mean square, per BS.1770.
[[nodiscard]] double lufsFromMeanSquare(double meanSquare);

}  // namespace zaro::render
