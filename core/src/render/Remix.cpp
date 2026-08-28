#include "zaro/core/render/Remix.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace zaro::render {
namespace {

/// The window the energy envelope is measured over, in seconds.
///
/// Ten milliseconds: short enough that a drum hit lands in one window rather
/// than being averaged with what follows it, long enough that a single sample
/// of noise is not an onset.
constexpr double kWindowSeconds = 0.01;

/// How far apart two onsets have to be to count as two.
///
/// A drum hit spreads energy over several windows, so without this every beat
/// arrives three or four times. 100 ms is faster than any music anybody would
/// cut to and slower than the smear of one hit.
constexpr double kMinimumGap = 0.1;

}  // namespace

Result<std::vector<double>> detectBeats(AudioSource& source, model::MediaRefId media,
                                        double seconds, const time::Rational& sampleRate) {
    if (seconds <= 0.0) {
        return Error{ErrorCode::InvalidData, "there is no audio to look at"};
    }
    const double rate = sampleRate.toDouble();
    const auto windowSamples = static_cast<std::int64_t>(std::llround(kWindowSeconds * rate));
    if (windowSamples <= 0) {
        return Error{ErrorCode::InvalidData, "that sample rate makes no sense"};
    }
    const auto windows = static_cast<std::int64_t>(seconds / kWindowSeconds);

    // The energy in each window, taken a second at a time so a long track does
    // not have to be held in memory all at once.
    std::vector<double> energy;
    energy.reserve(static_cast<std::size_t>(windows));
    media::AudioBuffer block;
    const std::int64_t blockSamples = windowSamples * 100;
    for (std::int64_t at = 0; at < windows * windowSamples; at += blockSamples) {
        if (Status read = source.read(media, time::RationalTime{at, sampleRate}, blockSamples,
                                      sampleRate, block);
            !read) {
            return read.error();
        }
        for (std::int64_t window = 0; window + windowSamples <= block.sampleCount();
             window += windowSamples) {
            double sum = 0.0;
            for (std::int32_t channel = 0; channel < block.channelCount(); ++channel) {
                const float* samples = block.channel(channel);
                for (std::int64_t i = 0; i < windowSamples; ++i) {
                    const double value = static_cast<double>(samples[window + i]);
                    sum += value * value;
                }
            }
            energy.push_back(std::sqrt(sum / static_cast<double>(windowSamples)));
        }
    }
    if (energy.size() < 4) {
        return Error{ErrorCode::InvalidData, "there is not enough audio to find a beat in"};
    }

    // Rises only: a fall in energy is the end of something, and no listener
    // claps on those.
    std::vector<double> rise(energy.size(), 0.0);
    double strongest = 0.0;
    for (std::size_t i = 1; i < energy.size(); ++i) {
        rise[i] = std::max(0.0, energy[i] - energy[i - 1]);
        strongest = std::max(strongest, rise[i]);
    }
    if (strongest <= 1e-6) {
        return Error{ErrorCode::InvalidData, "nothing in this audio starts or stops"};
    }

    // A peak is a rise bigger than its neighbours and worth noticing at all.
    // The threshold is a fraction of the strongest rise rather than an absolute
    // level, so a quiet track and a loud one give the same answer.
    const double threshold = strongest * 0.25;
    const auto gapWindows = static_cast<std::size_t>(kMinimumGap / kWindowSeconds);
    std::vector<double> beats;
    for (std::size_t i = 1; i + 1 < rise.size(); ++i) {
        if (rise[i] < threshold || rise[i] < rise[i - 1] || rise[i] < rise[i + 1]) {
            continue;
        }
        const double when = static_cast<double>(i) * kWindowSeconds;
        if (!beats.empty() && when - beats.back() < kMinimumGap) {
            // The louder of two hits too close together is the one that is
            // really there; the other is its smear.
            if (rise[i] > rise[static_cast<std::size_t>(beats.back() / kWindowSeconds)]) {
                beats.back() = when;
            }
            continue;
        }
        beats.push_back(when);
        static_cast<void>(gapWindows);
    }
    return beats;
}

Result<RemixPlan> planRemix(const std::vector<double>& beats, double sourceSeconds,
                            double targetSeconds) {
    if (targetSeconds <= 0.0 || sourceSeconds <= 0.0) {
        return Error{ErrorCode::InvalidData, "a length has to be more than nothing"};
    }
    if (targetSeconds >= sourceSeconds) {
        return Error{ErrorCode::InvalidData,
                     "this track is already shorter than that -- looping to extend it is a "
                     "different job"};
    }
    if (beats.size() < 3) {
        return Error{ErrorCode::InvalidData, "there are no beats in this track to cut on"};
    }

    const double toRemove = sourceSeconds - targetSeconds;

    // Every pair of beats whose gap is close to what has to go, scored by how
    // close the result lands to the length asked for. Both edges are beats, so
    // the join lands where the listener expects something to happen.
    double bestError = -1.0;
    RemixPlan best;
    for (std::size_t from = 1; from < beats.size(); ++from) {
        for (std::size_t to = from + 1; to < beats.size(); ++to) {
            const double gap = beats[to] - beats[from];
            const double error = std::fabs(gap - toRemove);
            if (bestError >= 0.0 && error >= bestError) {
                continue;
            }
            bestError = error;
            best.cutAt = beats[from];
            best.resumeFrom = beats[to];
            best.beatsRemoved = static_cast<int>(to - from);
        }
    }
    if (bestError < 0.0) {
        return Error{ErrorCode::InvalidData, "there is no pair of beats that far apart"};
    }

    best.seconds = sourceSeconds - (best.resumeFrom - best.cutAt);
    // Never longer than the piece it came from, and never so short that the
    // fade has nothing to happen in.
    best.joinFade = std::min(0.03, std::min(best.cutAt, sourceSeconds - best.resumeFrom) / 2.0);
    best.joinFade = std::max(0.0, best.joinFade);
    return best;
}

}  // namespace zaro::render
