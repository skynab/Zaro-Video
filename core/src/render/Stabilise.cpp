#include "zaro/core/render/Stabilise.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "zaro/core/render/Tracker.h"

namespace zaro::render {
namespace {

/// The middle value, or the mean of the middle two. Copies rather than sorting
/// in place: the caller's vector is reused frame to frame.
[[nodiscard]] double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t half = values.size() / 2;
    if (values.size() % 2 == 1) {
        return values[half];
    }
    return (values[half - 1] + values[half]) / 2.0;
}

/// A moving average over a window of frames, clamped at the ends.
///
/// **Clamped, not shortened.** A window that shrank towards the ends would
/// smooth the first and last frames less than the middle, so a clip would start
/// shaky, settle, and end shaky again -- which looks like the stabiliser giving
/// up rather than like a shot.
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

}  // namespace

Result<StabiliseResult> stabilise(FrameSource& source, model::MediaRefId media,
                                  const std::vector<time::RationalTime>& sourceTimes,
                                  const StabiliseOptions& options) {
    if (sourceTimes.size() < 3) {
        return Error{ErrorCode::InvalidData, "there is not enough of this clip to stabilise"};
    }
    const int perAxis = std::max(2, options.patchesPerAxis);

    // The camera path, as where the frame has got to relative to the first one.
    std::vector<double> pathX{0.0};
    std::vector<double> pathY{0.0};
    StabiliseResult result;

    auto first = source.imageFor(media, sourceTimes.front());
    if (!first) {
        return first.error();
    }
    RgbaImage previous = (*first)->clone();
    const std::int32_t width = previous.width();
    const std::int32_t height = previous.height();
    if (width < 32 || height < 32) {
        return Error{ErrorCode::InvalidData, "these frames are too small to stabilise"};
    }

    // Patch centres on an inset grid: a patch straddling the frame edge loses
    // half its samples the moment the picture moves, and the edge is where the
    // shake shows most.
    std::vector<std::pair<double, double>> centres;
    const double insetX = static_cast<double>(width) / static_cast<double>(perAxis + 1);
    const double insetY = static_cast<double>(height) / static_cast<double>(perAxis + 1);
    for (int row = 1; row <= perAxis; ++row) {
        for (int column = 1; column <= perAxis; ++column) {
            centres.emplace_back(insetX * column, insetY * row);
        }
    }

    PatchWindow window;
    window.halfWidth = insetX / 2.0;
    window.halfHeight = insetY / 2.0;
    window.search = std::clamp(static_cast<double>(width) * 0.03, 8.0, 48.0);

    std::vector<double> sawX;
    std::vector<double> sawY;
    for (std::size_t i = 1; i < sourceTimes.size(); ++i) {
        auto next = source.imageFor(media, sourceTimes[i]);
        if (!next) {
            return next.error();
        }
        RgbaImage current = (*next)->clone();

        sawX.clear();
        sawY.clear();
        for (const auto& [cx, cy] : centres) {
            window.centreX = cx;
            window.centreY = cy;
            const PatchTrack moved = trackPatch(previous, current, window);
            if (moved.usable) {
                sawX.push_back(moved.dx);
                sawY.push_back(moved.dy);
            }
        }
        // Fewer than half the patches agreeing on anything is a cut, a flash,
        // or a frame of fog. Stopping there and saying so beats carrying a
        // guess into every frame after it, since the path is cumulative.
        if (sawX.size() * 2 < centres.size()) {
            result.stopped =
                "the picture changed too much to keep following, at frame " + std::to_string(i);
            break;
        }
        pathX.push_back(pathX.back() + median(sawX));
        pathY.push_back(pathY.back() + median(sawY));
        ++result.measured;
        previous = std::move(current);
    }

    if (result.measured == 0) {
        // Nothing was followed, so there is nothing to hold still. Returning a
        // correction of zero would look like a clip that did not need
        // stabilising, which is a different and much more misleading answer
        // than "there is nothing in this picture to track".
        return Error{ErrorCode::InvalidData, result.stopped.empty()
                                                 ? "there is nothing in this clip to follow"
                                                 : result.stopped};
    }

    const auto rate = sourceTimes.front().rate().toDouble();
    const auto radius =
        static_cast<int>(std::max(1.0, std::round((options.smoothingSeconds * rate) / 2.0)));
    const std::vector<double> wantX = smoothed(pathX, radius);
    const std::vector<double> wantY = smoothed(pathY, radius);

    result.x.resize(pathX.size());
    result.y.resize(pathY.size());
    double worstX = 0.0;
    double worstY = 0.0;
    for (std::size_t i = 0; i < pathX.size(); ++i) {
        // Where the camera should have been, minus where it was. Moving the
        // picture the other way is what holds it still.
        result.x[i] = wantX[i] - pathX[i];
        result.y[i] = wantY[i] - pathY[i];
        worstX = std::max(worstX, std::fabs(result.x[i]));
        worstY = std::max(worstY, std::fabs(result.y[i]));
    }

    // Enough zoom that the largest correction still leaves the frame covered:
    // a shift of d exposes d on one side, so the picture has to grow by 2d.
    result.zoom = std::max(1.0 + ((2.0 * worstX) / static_cast<double>(width)),
                           1.0 + ((2.0 * worstY) / static_cast<double>(height)));
    return result;
}

}  // namespace zaro::render
