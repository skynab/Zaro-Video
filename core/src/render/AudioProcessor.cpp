#include "zaro/core/render/AudioProcessor.h"

#include <algorithm>
#include <cmath>

namespace zaro::render {
namespace {

constexpr double kPi = 3.14159265358979323846;

/// Below this the compressor is off rather than nearly off, and a filter at a
/// frequency this low is doing nothing a listener could hear.
constexpr double kNegligibleHz = 1.0;

double dbToLinear(double decibels) {
    return std::pow(10.0, decibels / 20.0);
}

double linearToDb(double value) {
    return value > 1e-9 ? 20.0 * std::log10(value) : -180.0;
}

/// The per-sample coefficient for a one-pole envelope with this time constant.
double timeCoefficient(double milliseconds, double sampleRate) {
    if (milliseconds <= 0.0 || sampleRate <= 0.0) {
        return 0.0;  // instant
    }
    return std::exp(-1.0 / ((milliseconds / 1000.0) * sampleRate));
}

}  // namespace

void Biquad::setBypass() {
    bypass_ = true;
    reset();
}

void Biquad::reset() {
    x1_ = 0.0;
    x2_ = 0.0;
    y1_ = 0.0;
    y2_ = 0.0;
}

void Biquad::setHighPass(double frequencyHz, double sampleRate, double q) {
    if (frequencyHz < kNegligibleHz || frequencyHz >= sampleRate * 0.5) {
        setBypass();
        return;
    }
    const double w0 = 2.0 * kPi * frequencyHz / sampleRate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cosine = std::cos(w0);
    const double a0 = 1.0 + alpha;

    b0_ = ((1.0 + cosine) / 2.0) / a0;
    b1_ = (-(1.0 + cosine)) / a0;
    b2_ = ((1.0 + cosine) / 2.0) / a0;
    a1_ = (-2.0 * cosine) / a0;
    a2_ = (1.0 - alpha) / a0;
    bypass_ = false;
    reset();
}

void Biquad::setLowPass(double frequencyHz, double sampleRate, double q) {
    if (frequencyHz < kNegligibleHz || frequencyHz >= sampleRate * 0.5) {
        setBypass();
        return;
    }
    const double w0 = 2.0 * kPi * frequencyHz / sampleRate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cosine = std::cos(w0);
    const double a0 = 1.0 + alpha;

    b0_ = ((1.0 - cosine) / 2.0) / a0;
    b1_ = (1.0 - cosine) / a0;
    b2_ = ((1.0 - cosine) / 2.0) / a0;
    a1_ = (-2.0 * cosine) / a0;
    a2_ = (1.0 - alpha) / a0;
    bypass_ = false;
    reset();
}

void Biquad::setPeaking(double frequencyHz, double sampleRate, double gainDb, double q) {
    if (frequencyHz < kNegligibleHz || frequencyHz >= sampleRate * 0.5 ||
        std::fabs(gainDb) < 0.01) {
        setBypass();
        return;
    }
    const double amplitude = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * frequencyHz / sampleRate;
    const double alpha = std::sin(w0) / (2.0 * std::max(0.05, q));
    const double cosine = std::cos(w0);
    const double a0 = 1.0 + (alpha / amplitude);

    b0_ = (1.0 + (alpha * amplitude)) / a0;
    b1_ = (-2.0 * cosine) / a0;
    b2_ = (1.0 - (alpha * amplitude)) / a0;
    a1_ = (-2.0 * cosine) / a0;
    a2_ = (1.0 - (alpha / amplitude)) / a0;
    bypass_ = false;
    reset();
}

float Biquad::process(float sample) {
    if (bypass_) {
        return sample;
    }
    const double x = static_cast<double>(sample);
    const double y = (b0_ * x) + (b1_ * x1_) + (b2_ * x2_) - (a1_ * y1_) - (a2_ * y2_);
    x2_ = x1_;
    x1_ = x;
    y2_ = y1_;
    y1_ = y;
    return static_cast<float>(y);
}

void TrackProcessor::configure(const model::AudioEq& eq, const model::Compressor& compressor,
                               double sampleRate, std::int32_t channelCount) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    compressor_ = compressor;
    attackCoefficient_ = timeCoefficient(compressor.attackMs, sampleRate_);
    releaseCoefficient_ = timeCoefficient(compressor.releaseMs, sampleRate_);

    eqActive_ = false;
    const std::int32_t channels = std::min(channelCount, kMaxChannels);
    for (std::int32_t channel = 0; channel < channels; ++channel) {
        auto& sections = filters_[static_cast<std::size_t>(channel)];
        if (!eq.enabled) {
            for (Biquad& section : sections) {
                section.setBypass();
            }
            continue;
        }
        sections[0].setHighPass(eq.highPassHz, sampleRate_);
        sections[1].setLowPass(eq.lowPassHz, sampleRate_);
        sections[2].setPeaking(eq.peakHz, sampleRate_, eq.peakGainDb, eq.peakQ);
        for (const Biquad& section : sections) {
            eqActive_ = eqActive_ || !section.isBypass();
        }
    }
}

void TrackProcessor::reset() {
    for (auto& sections : filters_) {
        for (Biquad& section : sections) {
            section.reset();
        }
    }
    envelope_ = 0.0;
    lastReductionDb_ = 0.0F;
}

void TrackProcessor::process(float* const* channels, std::int32_t channelCount,
                             std::int64_t sampleCount) {
    const std::int32_t count = std::min(channelCount, kMaxChannels);
    if (count <= 0 || sampleCount <= 0) {
        return;
    }

    if (eqActive_) {
        for (std::int32_t channel = 0; channel < count; ++channel) {
            auto& sections = filters_[static_cast<std::size_t>(channel)];
            float* samples = channels[channel];
            for (std::int64_t i = 0; i < sampleCount; ++i) {
                float value = samples[i];
                for (Biquad& section : sections) {
                    value = section.process(value);
                }
                samples[i] = value;
            }
        }
    }

    if (!compressor_.enabled || compressor_.ratio <= 1.0) {
        lastReductionDb_ = 0.0F;
        return;
    }

    const double threshold = dbToLinear(compressor_.thresholdDb);
    const double makeup = dbToLinear(compressor_.makeupDb);
    double worstReduction = 0.0;

    for (std::int64_t i = 0; i < sampleCount; ++i) {
        // One detector across all channels. Compressing each side separately
        // makes the image wander whenever one of them alone is loud, which is
        // the artefact that makes a mix sound unstable rather than controlled.
        double peak = 0.0;
        for (std::int32_t channel = 0; channel < count; ++channel) {
            peak = std::max(peak, static_cast<double>(std::fabs(channels[channel][i])));
        }

        // Attack when the signal is above the envelope, release when below:
        // the asymmetry is the whole behaviour, not a detail of it.
        const double coefficient = peak > envelope_ ? attackCoefficient_ : releaseCoefficient_;
        envelope_ = (coefficient * envelope_) + ((1.0 - coefficient) * peak);

        double gain = 1.0;
        if (envelope_ > threshold) {
            const double over = linearToDb(envelope_) - compressor_.thresholdDb;
            const double allowed = over / compressor_.ratio;
            gain = dbToLinear(allowed - over);
            worstReduction = std::min(worstReduction, allowed - over);
        }
        const auto applied = static_cast<float>(gain * makeup);
        for (std::int32_t channel = 0; channel < count; ++channel) {
            channels[channel][i] *= applied;
        }
    }
    lastReductionDb_ = static_cast<float>(worstReduction);
}

}  // namespace zaro::render
