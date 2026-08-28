#include "zaro/core/render/Loudness.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>

namespace zaro::render {
namespace {

constexpr double kPi = 3.14159265358979323846;

/// BS.1770's K-weighting, derived the way the standard derives it.
///
/// The published numbers — a corner of 1681.97 Hz, a Q of 0.70717 and a gain of
/// 3.9998 dB — look exactly like the parameters of a cookbook high-shelf, and
/// they are not. Building one from them gives a filter 0.2 dB low at 1 kHz,
/// which is enough to fail the standard's own calibration case: a 1 kHz sine at
/// -23 dBFS came out at -23.25 LUFS. The standard's derivation is below, and it
/// reproduces the published coefficients exactly.
constexpr double kShelfHz = 1681.974450955533;
constexpr double kShelfQ = 0.7071752369554196;
constexpr double kShelfGainDb = 3.999843853973347;
constexpr double kHighPassHz = 38.13547087602444;
constexpr double kHighPassQ = 0.5003270373238773;

void setKWeightingShelf(Biquad& filter, double sampleRate) {
    const double k = std::tan(kPi * kShelfHz / sampleRate);
    const double vh = std::pow(10.0, kShelfGainDb / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + (k / kShelfQ) + (k * k);
    filter.setCoefficients((vh + (vb * k / kShelfQ) + (k * k)) / a0, (2.0 * ((k * k) - vh)) / a0,
                           (vh - (vb * k / kShelfQ) + (k * k)) / a0, (2.0 * ((k * k) - 1.0)) / a0,
                           (1.0 - (k / kShelfQ) + (k * k)) / a0);
}

void setKWeightingHighPass(Biquad& filter, double sampleRate) {
    const double k = std::tan(kPi * kHighPassHz / sampleRate);
    const double denominator = 1.0 + (k / kHighPassQ) + (k * k);
    filter.setCoefficients(1.0, -2.0, 1.0, (2.0 * ((k * k) - 1.0)) / denominator,
                           (1.0 - (k / kHighPassQ) + (k * k)) / denominator);
}

/// Channel weights. The standard gives the surround channels more; stereo and
/// mono are all ones, which is what this handles.
double channelWeight(std::int32_t channel) {
    // Left, right and centre count fully. Anything beyond a five-channel layout
    // is not weighted here, and a mix that wide would need the surround
    // coefficients written out with it.
    return channel < 3 ? 1.0 : 1.41;
}

}  // namespace

double lufsFromMeanSquare(double meanSquare) {
    if (!(meanSquare > 0.0)) {
        return LoudnessMeter::kSilence;
    }
    // The -0.691 is the standard's calibration offset: it is what makes a
    // 1 kHz sine at -23 dBFS read -23 LUFS rather than something close to it.
    return -0.691 + (10.0 * std::log10(meanSquare));
}

void LoudnessMeter::configure(double sampleRate, std::int32_t channelCount) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    channels_ = std::clamp(channelCount, 1, kMaxChannels);
    blockSamples_ = static_cast<std::int64_t>(std::llround(sampleRate_ * 0.4));
    stepSamples_ = std::max<std::int64_t>(1, blockSamples_ / 4);

    for (std::int32_t channel = 0; channel < channels_; ++channel) {
        setKWeightingShelf(shelf_[static_cast<std::size_t>(channel)], sampleRate_);
        setKWeightingHighPass(highPass_[static_cast<std::size_t>(channel)], sampleRate_);
    }
    reset();
}

void LoudnessMeter::reset() {
    for (std::int32_t channel = 0; channel < channels_; ++channel) {
        shelf_[static_cast<std::size_t>(channel)].reset();
        highPass_[static_cast<std::size_t>(channel)].reset();
    }
    window_.assign(static_cast<std::size_t>(std::max<std::int64_t>(1, blockSamples_)), 0.0);
    windowFill_ = 0;
    sinceLastBlock_ = 0;
    blocks_.clear();
    peak_ = 0.0;
}

void LoudnessMeter::pushBlock(double meanSquare) {
    blocks_.push_back(meanSquare);
}

void LoudnessMeter::feed(const float* const* channels, std::int32_t channelCount,
                         std::int64_t sampleCount) {
    if (blockSamples_ <= 0 || sampleCount <= 0) {
        return;
    }
    const std::int32_t count = std::min({channelCount, channels_, kMaxChannels});

    for (std::int64_t i = 0; i < sampleCount; ++i) {
        double weighted = 0.0;
        for (std::int32_t channel = 0; channel < count; ++channel) {
            const float raw = channels[channel][i];
            peak_ = std::max(peak_, static_cast<double>(std::fabs(raw)));
            const float shelved = shelf_[static_cast<std::size_t>(channel)].process(raw);
            const float filtered = highPass_[static_cast<std::size_t>(channel)].process(shelved);
            const double value = static_cast<double>(filtered);
            weighted += channelWeight(channel) * value * value;
        }

        // A ring of the last 400 ms of weighted squares, with a block emitted
        // every 100 ms -- the standard's 75% overlap.
        window_[static_cast<std::size_t>(windowFill_ % blockSamples_)] = weighted;
        ++windowFill_;
        ++sinceLastBlock_;
        if (windowFill_ >= blockSamples_ && sinceLastBlock_ >= stepSamples_) {
            const double sum = std::accumulate(window_.begin(), window_.end(), 0.0);
            pushBlock(sum / static_cast<double>(blockSamples_));
            sinceLastBlock_ = 0;
        }
    }
}

double LoudnessMeter::momentaryLufs() const {
    return blocks_.empty() ? kSilence : lufsFromMeanSquare(blocks_.back());
}

double LoudnessMeter::shortTermLufs() const {
    if (blocks_.empty()) {
        return kSilence;
    }
    // Three seconds is thirty of these blocks, or everything so far if less has
    // been measured.
    const std::size_t wanted = static_cast<std::size_t>(3 * kBlocksPerSecond);
    const std::size_t take = std::min(wanted, blocks_.size());
    const double sum =
        std::accumulate(blocks_.end() - static_cast<std::ptrdiff_t>(take), blocks_.end(), 0.0);
    return lufsFromMeanSquare(sum / static_cast<double>(take));
}

double LoudnessMeter::integratedLufs() const {
    if (blocks_.empty()) {
        return kSilence;
    }

    // The absolute gate first: anything below -70 LUFS is not programme.
    std::vector<double> keep;
    keep.reserve(blocks_.size());
    for (const double block : blocks_) {
        if (lufsFromMeanSquare(block) > kSilence) {
            keep.push_back(block);
        }
    }
    if (keep.empty()) {
        return kSilence;
    }

    // Then the relative gate, ten below the ungated mean. This is the part that
    // matters: without it a programme with quiet passages measures quieter than
    // it sounds, and everyone pushes the loud parts up to compensate.
    const double ungated =
        std::accumulate(keep.begin(), keep.end(), 0.0) / static_cast<double>(keep.size());
    const double threshold = lufsFromMeanSquare(ungated) - 10.0;

    double sum = 0.0;
    std::size_t used = 0;
    for (const double block : keep) {
        if (lufsFromMeanSquare(block) > threshold) {
            sum += block;
            ++used;
        }
    }
    if (used == 0) {
        return lufsFromMeanSquare(ungated);
    }
    return lufsFromMeanSquare(sum / static_cast<double>(used));
}

double LoudnessMeter::samplePeakDbfs() const {
    return peak_ > 0.0 ? 20.0 * std::log10(peak_) : -180.0;
}

}  // namespace zaro::render
