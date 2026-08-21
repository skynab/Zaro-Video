#include "zaro/core/render/SceneDetect.h"

#include <algorithm>
#include <cmath>

namespace zaro::render {
namespace {

constexpr std::size_t kBins = 16;

/// Linear light to a bin index, through the same warp the curve tables use.
///
/// Binning linear values directly would put almost every pixel of an ordinary
/// picture in the bottom two bins -- linear light is mostly small numbers --
/// and two quite different shots would look identical. The warp spreads the
/// range the way the eye does, and it is the one this renderer already uses to
/// index a table by brightness, so there is one answer to "where does this
/// value sit" rather than two.
std::size_t binFor(float value) {
    const float lifted = std::max(value, 0.0F);
    const float index = std::sqrt(lifted / (1.0F + lifted));
    const auto bin = static_cast<std::size_t>(index * static_cast<float>(kBins));
    return std::min(bin, kBins - 1);
}

}  // namespace

void sceneHistogram(const RgbaImage& frame, std::array<double, 48>& out) {
    out.fill(0.0);
    if (!frame.isValid()) {
        return;
    }
    // Every fourth pixel in each direction. A histogram is a summary, and
    // sixteen times fewer samples changes it by less than the threshold cares
    // about while making the analysis of a long clip take a sixteenth as long.
    constexpr std::int32_t kStep = 4;
    double counted = 0.0;
    for (std::int32_t y = 0; y < frame.height(); y += kStep) {
        const Rgba* row = frame.row(y);
        for (std::int32_t x = 0; x < frame.width(); x += kStep) {
            const Rgba& pixel = row[x];
            // Un-premultiplied, so a fade to transparent is not read as a
            // change of colour.
            const float alpha = pixel.a > 0.0001F ? pixel.a : 1.0F;
            out[binFor(pixel.r / alpha)] += 1.0;
            out[kBins + binFor(pixel.g / alpha)] += 1.0;
            out[(2 * kBins) + binFor(pixel.b / alpha)] += 1.0;
            counted += 1.0;
        }
    }
    if (counted <= 0.0) {
        return;
    }
    // Normalised per channel, so the measure does not depend on the frame size.
    const double scale = 1.0 / counted;
    for (double& bin : out) {
        bin *= scale;
    }
}

double histogramDistance(const std::array<double, 48>& a, const std::array<double, 48>& b) {
    // Total variation: half the sum of the absolute differences, which for two
    // distributions that each sum to one lands between 0 and 1. It answers
    // exactly the question being asked -- what fraction of the picture moved.
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        sum += std::fabs(a[i] - b[i]);
    }
    // Three channels, each summing to 1, so the total is up to 6 rather than 2.
    return std::clamp(sum / 6.0, 0.0, 1.0);
}

void SceneDetector::accept(const time::RationalTime& at, double confidence) {
    // The start of the material counts as the start of a shot, so a candidate
    // a frame or two in is measured against it and refused. Without this, a
    // clip whose first frame differs from its second -- a flash on the head, a
    // black frame the encoder left there -- gets split one frame from its own
    // beginning, which is not an edit anybody can use.
    const time::RationalTime previousCut = cuts_.empty() ? firstTime_ : cuts_.back().at;
    const time::RationalTime since = at - previousCut.rescaledTo(at.rate());
    if (since < options_.minimumShot.rescaledTo(at.rate())) {
        // One transition reported as several. The first frame that changed is
        // the cut; the rest of the dissolve is the same event.
        return;
    }
    cuts_.push_back(SceneCut{at, confidence});
}

void SceneDetector::push(const RgbaImage& frame, const time::RationalTime& at) {
    if (!hasPrevious_) {
        firstTime_ = at;
    }
    Histogram current{};
    sceneHistogram(frame, current);

    // Candidates waiting to be confirmed: a cut is still a cut a few frames
    // later, and a flash is not.
    for (auto it = pending_.begin(); it != pending_.end();) {
        ++it->framesSeen;
        if (it->framesSeen < options_.confirmAfter) {
            ++it;
            continue;
        }
        const double stillDifferent = histogramDistance(it->before, current);
        if (stillDifferent >= options_.threshold) {
            accept(it->at, it->confidence);
        }
        it = pending_.erase(it);
    }

    if (hasPrevious_) {
        const double distance = histogramDistance(previous_, current);
        if (distance >= options_.threshold) {
            if (options_.confirmAfter <= 0) {
                accept(at, distance);
            } else {
                // The oldest frame still in hand, which is a few frames before
                // the boundary. Using the frame immediately before it would
                // mean measuring a flash against the flash.
                pending_.push_back(Pending{at, distance, recent_.front(), 0});
            }
        }
    }

    previous_ = current;
    hasPrevious_ = true;
    recent_.push_back(current);
    while (recent_.size() > static_cast<std::size_t>(std::max(1, options_.confirmAfter + 1))) {
        recent_.pop_front();
    }
}

void SceneDetector::flush() {
    // A cut in the last few frames has nothing after it to confirm against.
    // The evidence for it is the same as for any other; only the confirmation
    // is missing.
    for (const Pending& candidate : pending_) {
        accept(candidate.at, candidate.confidence);
    }
    pending_.clear();
}

}  // namespace zaro::render
