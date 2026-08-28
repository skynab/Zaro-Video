#include "zaro/core/media/AudioAlign.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace zaro::media {

namespace {

/// Pearson correlation of two equal-length spans.
///
/// Normalised rather than a plain dot product, so that the answer does not
/// simply follow whichever part of the signal is loudest: two cameras at
/// different distances from the same clap record the same shape at different
/// levels, and it is the shape that says where they line up.
double correlate(const double* a, const double* b, std::int64_t count) {
    if (count <= 1) {
        return 0.0;
    }
    const auto n = static_cast<double>(count);
    double sumA = 0.0;
    double sumB = 0.0;
    for (std::int64_t i = 0; i < count; ++i) {
        sumA += a[i];
        sumB += b[i];
    }
    const double meanA = sumA / n;
    const double meanB = sumB / n;

    double covariance = 0.0;
    double varianceA = 0.0;
    double varianceB = 0.0;
    for (std::int64_t i = 0; i < count; ++i) {
        const double da = a[i] - meanA;
        const double db = b[i] - meanB;
        covariance += da * db;
        varianceA += da * da;
        varianceB += db * db;
    }
    if (varianceA <= 0.0 || varianceB <= 0.0) {
        // One of them is a constant -- silence, or a held tone. There is
        // nothing in it to line up against.
        return 0.0;
    }
    return covariance / std::sqrt(varianceA * varianceB);
}

/// The same, for raw samples, at one lag.
double correlateSamples(const float* a, const float* b, std::int64_t count) {
    if (count <= 1) {
        return 0.0;
    }
    double dot = 0.0;
    double normA = 0.0;
    double normB = 0.0;
    for (std::int64_t i = 0; i < count; ++i) {
        const double da = static_cast<double>(a[i]);
        const double db = static_cast<double>(b[i]);
        dot += da * db;
        normA += da * da;
        normB += db * db;
    }
    if (normA <= 0.0 || normB <= 0.0) {
        return 0.0;
    }
    return dot / std::sqrt(normA * normB);
}

}  // namespace

std::vector<double> envelope(const float* samples, std::int64_t count, std::int64_t blockSamples) {
    std::vector<double> out;
    if (samples == nullptr || count <= 0 || blockSamples <= 0) {
        return out;
    }
    out.reserve(static_cast<std::size_t>((count + blockSamples - 1) / blockSamples));
    for (std::int64_t start = 0; start < count; start += blockSamples) {
        const std::int64_t stop = std::min(start + blockSamples, count);
        double sum = 0.0;
        for (std::int64_t i = start; i < stop; ++i) {
            const double value = static_cast<double>(samples[i]);
            sum += value * value;
        }
        out.push_back(std::sqrt(sum / static_cast<double>(stop - start)));
    }
    return out;
}

Alignment align(const float* reference, std::int64_t referenceCount, const float* other,
                std::int64_t otherCount, const AlignOptions& options) {
    Alignment result;
    if (reference == nullptr || other == nullptr || referenceCount <= 0 || otherCount <= 0 ||
        options.blockSamples <= 0) {
        result.reason = "there is nothing to compare";
        return result;
    }

    const std::vector<double> envA = envelope(reference, referenceCount, options.blockSamples);
    const std::vector<double> envB = envelope(other, otherCount, options.blockSamples);
    const auto blocksA = static_cast<std::int64_t>(envA.size());
    const auto blocksB = static_cast<std::int64_t>(envB.size());
    if (blocksA <= 1 || blocksB <= 1) {
        result.reason = "there is not enough audio to compare";
        return result;
    }

    const std::int64_t maxLagBlocks =
        std::max<std::int64_t>(1, options.maxLagSamples / options.blockSamples);
    // A lag that leaves the two barely touching will correlate perfectly over
    // the handful of blocks that overlap, and mean nothing. A quarter of the
    // shorter signal is enough to be an answer about the recording rather than
    // about its edge.
    const std::int64_t minimumOverlap = std::max<std::int64_t>(2, std::min(blocksA, blocksB) / 4);

    double bestScore = -2.0;
    std::int64_t bestLag = 0;
    bool found = false;
    std::vector<double> windowA;
    std::vector<double> windowB;
    for (std::int64_t lag = -maxLagBlocks; lag <= maxLagBlocks; ++lag) {
        // envB[k + lag] against envA[k]: a positive lag means the moment that
        // is at k in the reference is further into the other recording, which
        // is what the offset means to the caller.
        const std::int64_t first = std::max<std::int64_t>(0, -lag);
        const std::int64_t last = std::min(blocksA, blocksB - lag);
        const std::int64_t overlap = last - first;
        if (overlap < minimumOverlap) {
            continue;
        }
        windowA.assign(envA.begin() + first, envA.begin() + last);
        windowB.assign(envB.begin() + (first + lag), envB.begin() + (last + lag));
        const double score = correlate(windowA.data(), windowB.data(), overlap);
        if (score > bestScore) {
            bestScore = score;
            bestLag = lag;
            found = true;
        }
    }
    if (!found) {
        result.reason = "the two never overlap by enough to compare";
        return result;
    }
    if (bestScore <= 0.0) {
        result.reason = "the two recordings have nothing in common";
        return result;
    }

    result.offsetSamples = bestLag * options.blockSamples;
    result.confidence = std::clamp(bestScore, 0.0, 1.0);

    // --- Refinement --------------------------------------------------------
    // The coarse answer is only as precise as a block. Correlate the raw
    // samples over a short window to find the exact sample, searching one block
    // either side of what the envelope said.
    const std::int64_t window = options.refineWindowSamples;
    if (window <= 0) {
        return result;
    }

    // The loudest block of the overlap, so the window has something in it.
    std::int64_t loudest = std::max<std::int64_t>(0, -bestLag);
    double loudestValue = -1.0;
    const std::int64_t firstBlock = std::max<std::int64_t>(0, -bestLag);
    const std::int64_t lastBlock = std::min(blocksA, blocksB - bestLag);
    for (std::int64_t k = firstBlock; k < lastBlock; ++k) {
        const double level = envA[static_cast<std::size_t>(k)];
        if (level > loudestValue) {
            loudestValue = level;
            loudest = k;
        }
    }

    std::int64_t start = (loudest * options.blockSamples) - (window / 2);
    start = std::clamp<std::int64_t>(start, 0, std::max<std::int64_t>(0, referenceCount - window));
    const std::int64_t length = std::min(window, referenceCount - start);
    if (length <= 1) {
        return result;
    }

    double refinedScore = -2.0;
    std::int64_t refinedLag = result.offsetSamples;
    for (std::int64_t lag = result.offsetSamples - options.blockSamples;
         lag <= result.offsetSamples + options.blockSamples; ++lag) {
        const std::int64_t otherStart = start + lag;
        if (otherStart < 0 || otherStart + length > otherCount) {
            continue;
        }
        const double score = correlateSamples(reference + start, other + otherStart, length);
        if (score > refinedScore) {
            refinedScore = score;
            refinedLag = lag;
        }
    }
    if (refinedScore > 0.0) {
        result.offsetSamples = refinedLag;
        // The refined figure is the one to report: it was measured where the
        // signal actually is, over the samples themselves.
        result.confidence = std::clamp(refinedScore, 0.0, 1.0);
    }
    return result;
}

}  // namespace zaro::media
