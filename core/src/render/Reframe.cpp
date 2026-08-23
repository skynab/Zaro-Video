#include "zaro/core/render/Reframe.h"

#include <algorithm>
#include <cmath>

namespace zaro::render {
namespace {

/// The same clamped moving average the stabiliser uses, and for the same
/// reason: a window that shrank towards the ends would leave a clip starting
/// and finishing jittery.
[[nodiscard]] std::vector<double> smoothed(const std::vector<double>& path, int radius) {
    std::vector<double> out(path.size(), 0.0);
    const auto count = static_cast<std::int64_t>(path.size());
    for (std::int64_t i = 0; i < count; ++i) {
        double sum = 0.0;
        for (std::int64_t at = i - radius; at <= i + radius; ++at) {
            sum += path[static_cast<std::size_t>(std::clamp<std::int64_t>(at, 0, count - 1))];
        }
        out[static_cast<std::size_t>(i)] = sum / static_cast<double>((2 * radius) + 1);
    }
    return out;
}

[[nodiscard]] double luma(const Rgba& pixel) {
    return (0.2126 * static_cast<double>(pixel.r)) + (0.7152 * static_cast<double>(pixel.g)) +
           (0.0722 * static_cast<double>(pixel.b));
}

/// Where the detail is, along each axis.
///
/// One pass, accumulating the absolute difference between neighbouring pixels
/// into both a column total and a row total. Two profiles rather than a full
/// saliency map: the window can only move along one axis at a time anyway, and
/// a profile is a thousand numbers where a map is a million.
struct Interest {
    std::vector<double> columns;
    std::vector<double> rows;
};

[[nodiscard]] Interest interestOf(const RgbaImage& image) {
    Interest found;
    found.columns.assign(static_cast<std::size_t>(image.width()), 0.0);
    found.rows.assign(static_cast<std::size_t>(image.height()), 0.0);
    for (std::int32_t y = 1; y < image.height(); ++y) {
        const Rgba* row = image.row(y);
        const Rgba* above = image.row(y - 1);
        for (std::int32_t x = 1; x < image.width(); ++x) {
            const double here = luma(row[x]);
            const double energy =
                std::fabs(here - luma(row[x - 1])) + std::fabs(here - luma(above[x]));
            found.columns[static_cast<std::size_t>(x)] += energy;
            found.rows[static_cast<std::size_t>(y)] += energy;
        }
    }
    return found;
}

/// Where to put the window of `span` samples: on the interest, not merely
/// around it.
///
/// The window with the most interest in it is found first, with a running sum
/// rather than a search per position -- a quadratic scan of a 4K frame per
/// frame is the difference between a reframe somebody waits for and one they
/// abandon. Then the *centroid of the interest inside that window* is what the
/// window is centred on.
///
/// That second step is not a refinement, it is the answer. A subject smaller
/// than the window leaves every window containing it tied on sum, and a plain
/// maximum picks whichever tie the loop happened to see first -- which is the
/// leftmost, so a subject a quarter of the way across a frame ended up at its
/// right-hand edge. The centroid puts it in the middle, which is what "reframe
/// on the subject" means.
[[nodiscard]] double bestCentre(const std::vector<double>& profile, double span, bool& sawAny) {
    const auto count = static_cast<std::int64_t>(profile.size());
    const auto width =
        std::clamp(static_cast<std::int64_t>(std::llround(span)), std::int64_t{1}, count);
    double total = 0.0;
    for (std::int64_t i = 0; i < width; ++i) {
        total += profile[static_cast<std::size_t>(i)];
    }
    double best = total;
    std::int64_t bestStart = 0;
    double everything = total;
    for (std::int64_t start = 1; start + width <= count; ++start) {
        total -= profile[static_cast<std::size_t>(start - 1)];
        total += profile[static_cast<std::size_t>(start + width - 1)];
        everything += profile[static_cast<std::size_t>(start + width - 1)];
        if (total > best) {
            best = total;
            bestStart = start;
        }
    }

    const double half = static_cast<double>(width) / 2.0;
    sawAny = everything > 1e-6 && best > 1e-6;
    if (!sawAny) {
        // Nothing anywhere: the middle is the only defensible answer.
        return static_cast<double>(count) / 2.0;
    }

    double weighted = 0.0;
    double weight = 0.0;
    for (std::int64_t at = bestStart; at < bestStart + width; ++at) {
        const double share = profile[static_cast<std::size_t>(at)];
        weighted += (static_cast<double>(at) + 0.5) * share;
        weight += share;
    }
    const double centre = weight > 0.0 ? weighted / weight : static_cast<double>(bestStart) + half;
    // Kept inside the picture: a window hanging off the edge would show
    // nothing there, and filling the frame is the one thing not negotiable.
    return std::clamp(centre, half, static_cast<double>(count) - half);
}

}  // namespace

Result<ReframeResult> autoReframe(FrameSource& source, model::MediaRefId media,
                                  const std::vector<time::RationalTime>& sourceTimes,
                                  std::int32_t targetWidth, std::int32_t targetHeight,
                                  const ReframeOptions& options) {
    if (sourceTimes.empty()) {
        return Error{ErrorCode::InvalidData, "there is nothing to reframe"};
    }
    if (targetWidth <= 0 || targetHeight <= 0) {
        return Error{ErrorCode::InvalidData, "the frame to fit has no size"};
    }

    auto first = source.imageFor(media, sourceTimes.front());
    if (!first) {
        return first.error();
    }
    const std::int32_t sourceWidth = (*first)->width();
    const std::int32_t sourceHeight = (*first)->height();
    if (sourceWidth <= 0 || sourceHeight <= 0) {
        return Error{ErrorCode::InvalidData, "that clip has no picture"};
    }

    ReframeResult result;
    // Cover, never contain: an empty edge is a mistake rather than a look.
    result.scale = std::max(static_cast<double>(targetWidth) / static_cast<double>(sourceWidth),
                            static_cast<double>(targetHeight) / static_cast<double>(sourceHeight));

    // How much of the source is visible once it is scaled to cover, in source
    // pixels. Whichever axis has more than the frame is the one with slack.
    const double visibleWidth = static_cast<double>(targetWidth) / result.scale;
    const double visibleHeight = static_cast<double>(targetHeight) / result.scale;

    std::vector<double> centresX;
    std::vector<double> centresY;
    bool anyInterest = false;
    for (const time::RationalTime& at : sourceTimes) {
        auto frame = source.imageFor(media, at);
        if (!frame) {
            if (centresX.empty()) {
                return frame.error();
            }
            // A frame that will not read holds the last decision rather than
            // ending the reframe: a gap in the middle of a shot should not
            // leave the second half uncomposed.
            centresX.push_back(centresX.back());
            centresY.push_back(centresY.back());
            continue;
        }
        const Interest interest = interestOf(**frame);
        bool sawX = false;
        bool sawY = false;
        centresX.push_back(bestCentre(interest.columns, visibleWidth, sawX));
        centresY.push_back(bestCentre(interest.rows, visibleHeight, sawY));
        anyInterest = anyInterest || sawX || sawY;
        ++result.measured;
    }

    const auto rate = sourceTimes.front().rate().toDouble();
    const auto radius =
        static_cast<int>(std::max(1.0, std::round((options.smoothingSeconds * rate) / 2.0)));
    const std::vector<double> pathX = smoothed(centresX, radius);
    const std::vector<double> pathY = smoothed(centresY, radius);

    result.x.resize(pathX.size());
    result.y.resize(pathY.size());
    for (std::size_t i = 0; i < pathX.size(); ++i) {
        // Where the chosen centre is, relative to the middle of the source,
        // moved the other way and scaled into output pixels.
        const double offsetX = pathX[i] - (static_cast<double>(sourceWidth) / 2.0);
        const double offsetY = pathY[i] - (static_cast<double>(sourceHeight) / 2.0);
        result.x[i] = -offsetX * result.scale;
        result.y[i] = -offsetY * result.scale;
    }
    if (!anyInterest) {
        result.reason = "nothing in this shot stands out, so it is centred";
    }
    return result;
}

}  // namespace zaro::render
