#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "zaro/core/Error.h"
#include "zaro/core/media/AudioBuffer.h"
#include "zaro/core/time/Rational.h"

namespace zaro::media {

/// Minimum and maximum sample over a span, per channel.
///
/// A waveform is drawn as the envelope between these two, not as an average.
/// Averaging a symmetric signal gives approximately zero everywhere and draws a
/// flat line: the peaks are the entire content of the picture.
struct WaveformBucket {
    float minimum{0.0F};
    float maximum{0.0F};

    friend bool operator==(const WaveformBucket&, const WaveformBucket&) = default;
};

/// Peak data for one audio stream, at one resolution.
class Waveform {
public:
    Waveform() = default;
    Waveform(std::int32_t channelCount, std::int64_t samplesPerBucket, time::Rational sampleRate);

    [[nodiscard]] bool isValid() const noexcept {
        return channelCount_ > 0 && samplesPerBucket_ > 0;
    }
    [[nodiscard]] std::int32_t channelCount() const noexcept { return channelCount_; }
    [[nodiscard]] std::int64_t samplesPerBucket() const noexcept { return samplesPerBucket_; }
    [[nodiscard]] const time::Rational& sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] std::int64_t bucketCount() const noexcept {
        return channelCount_ > 0 ? static_cast<std::int64_t>(buckets_.size()) / channelCount_ : 0;
    }

    [[nodiscard]] const WaveformBucket& at(std::int32_t channel, std::int64_t bucket) const;

    /// Total audio covered.
    [[nodiscard]] time::RationalTime duration() const;

    /// Fold this waveform down to `count` buckets spanning `[first, last)` of
    /// the original. Drawing asks for one bucket per pixel, and reducing from
    /// an existing waveform is far cheaper than re-reading the file at every
    /// zoom level.
    [[nodiscard]] Waveform resampled(std::int64_t first, std::int64_t last,
                                     std::int64_t count) const;

    /// Accumulate a block of audio. Blocks may be any length; bucket boundaries
    /// are tracked across calls, so a waveform built from one long read and one
    /// built from many short reads are identical.
    void append(const AudioBuffer& block);
    /// Close the final, partially filled bucket.
    void finish();

    // --- On-disk form -------------------------------------------------------

    [[nodiscard]] std::string encode() const;
    [[nodiscard]] static Result<Waveform> decode(const std::string& bytes);

private:
    std::vector<WaveformBucket> buckets_;  ///< Channel-major within each bucket.
    std::int32_t channelCount_{0};
    std::int64_t samplesPerBucket_{0};
    time::Rational sampleRate_{48000, 1};

    // Carried across append() calls so block boundaries do not create buckets.
    std::vector<WaveformBucket> pending_;
    std::int64_t pendingSamples_{0};
};

/// A cheap identity for a media file, for use as a cache key.
///
/// Size, modification time and a sample of the bytes at each end. Deliberately
/// not a hash of the whole file: hashing a hundred gigabytes of camera media on
/// import would make the application unusable, and the cost of being wrong here
/// is a stale waveform, not a corrupted project. Anything that needs real
/// identity -- relinking, conform -- should not use this.
[[nodiscard]] Result<std::string> quickContentHash(const std::string& path);

}  // namespace zaro::media
