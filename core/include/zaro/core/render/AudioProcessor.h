#pragma once

#include <array>
#include <cstdint>

#include "zaro/core/model/AudioProcessing.h"
#include "zaro/core/time/Rational.h"

namespace zaro::render {

/// One biquad section, in direct form I.
///
/// Coefficients from the RBJ cookbook, which is what everything else uses --
/// so a filter set to 80 Hz here and 80 Hz somewhere else is the same filter,
/// and a plugin's numbers mean what they say.
class Biquad {
public:
    void setBypass();
    void setHighPass(double frequencyHz, double sampleRate, double q = 0.7071);
    void setLowPass(double frequencyHz, double sampleRate, double q = 0.7071);
    void setPeaking(double frequencyHz, double sampleRate, double gainDb, double q);
    /// A shelf lifting everything above the corner. Needed by the loudness
    /// meter: BS.1770's first K-weighting stage is exactly this filter, with
    /// the standard's own corner, Q and gain.
    void setHighShelf(double frequencyHz, double sampleRate, double gainDb, double q);

    /// Set the coefficients directly, for a filter whose design is specified
    /// somewhere other than here — BS.1770's K-weighting, whose derivation is
    /// its own and does not come out of the cookbook forms above.
    void setCoefficients(double b0, double b1, double b2, double a1, double a2);

    [[nodiscard]] float process(float sample);
    /// Forget the past. A filter's delay line is the last two samples it saw,
    /// and after a seek those came from somewhere else entirely.
    void reset();

    [[nodiscard]] bool isBypass() const noexcept { return bypass_; }

private:
    bool bypass_{true};
    double b0_{1.0};
    double b1_{0.0};
    double b2_{0.0};
    double a1_{0.0};
    double a2_{0.0};
    double x1_{0.0};
    double x2_{0.0};
    double y1_{0.0};
    double y2_{0.0};
};

/// A track's processing chain: equaliser then compressor.
///
/// **This has state, so mixing is no longer a pure function of time.** A
/// filter's delay line and a compressor's envelope both depend on what came
/// immediately before, which is the point of them — but it means a seek has to
/// reset the chain, or the envelope from one part of the timeline follows the
/// playhead to another. `AudioGraph::resetProcessing` is that reset, and
/// anything that moves the playhead has to call it.
class TrackProcessor {
public:
    void configure(const model::AudioEq& eq, const model::Compressor& compressor, double sampleRate,
                   std::int32_t channelCount);
    void reset();

    /// Process in place. Channels are processed with a shared gain envelope so
    /// the stereo image does not wander when one side alone is loud.
    void process(float* const* channels, std::int32_t channelCount, std::int64_t sampleCount);

    /// The most gain reduction applied in the last block, in decibels. Never
    /// positive.
    [[nodiscard]] float lastReductionDb() const noexcept { return lastReductionDb_; }

private:
    static constexpr std::int32_t kMaxChannels = 8;
    static constexpr std::int32_t kSections = 3;

    std::array<std::array<Biquad, kSections>, kMaxChannels> filters_{};
    model::Compressor compressor_;
    double sampleRate_{48000.0};
    double attackCoefficient_{0.0};
    double releaseCoefficient_{0.0};
    double envelope_{0.0};
    float lastReductionDb_{0.0F};
    bool eqActive_{false};
};

}  // namespace zaro::render
