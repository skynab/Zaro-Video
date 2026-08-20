#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace zaro::media {

/// Where one recording of a sound sits relative to another.
struct Alignment {
    /// How much later the same moment arrives in the second signal, in samples.
    /// Negative means it arrives earlier.
    std::int64_t offsetSamples{0};

    /// The normalised correlation at the answer, from 0 to 1.
    ///
    /// Reported rather than thresholded here, because what counts as good
    /// enough depends on what is being done with it: writing an offset into a
    /// multicam clip wants a high bar, showing a suggestion next to a number
    /// somebody can overrule wants a low one.
    double confidence{0.0};

    /// Empty when the answer is usable. Set when it is not -- silence, no
    /// overlap -- so a caller can say why rather than reporting a confident
    /// zero.
    std::string_view reason;
};

/// Options for `align`, in samples so the function has no opinion about rates.
struct AlignOptions {
    /// How far apart the two recordings may have started. The search costs
    /// time proportional to this, and a range wider than the cameras could
    /// plausibly differ by only adds chances to find a spurious peak.
    std::int64_t maxLagSamples{0};

    /// Samples per envelope block. The coarse search works at this resolution;
    /// the refinement then works at single samples.
    std::int64_t blockSamples{0};

    /// How many samples the refinement pass correlates. Longer is more
    /// certain and quadratically slower.
    std::int64_t refineWindowSamples{0};
};

/// Find the offset between two mono signals.
///
/// **Two passes, not one.** A single correlation at sample resolution over a
/// thirty-second search range is hundreds of billions of operations; one over a
/// loudness envelope is millions, but its answer is only as precise as the
/// block it works in -- and a tenth of a frame of error is exactly the kind of
/// sync mistake nobody can find later, because it looks fine on most cuts. So
/// the envelope finds roughly where, and a short correlation of the raw samples
/// around that answer finds exactly where.
///
/// **The refinement listens where there is something to hear.** Its window is
/// centred on the loudest part of the overlap rather than on the middle,
/// because a second of room tone correlates with any other second of room tone
/// and would confidently return the coarse answer unchanged.
[[nodiscard]] Alignment align(const float* reference, std::int64_t referenceCount,
                              const float* other, std::int64_t otherCount,
                              const AlignOptions& options);

/// Block RMS of a signal. Exposed because it is worth testing on its own, and
/// because a caller with an envelope already computed should not make another.
[[nodiscard]] std::vector<double> envelope(const float* samples, std::int64_t count,
                                           std::int64_t blockSamples);

}  // namespace zaro::media
