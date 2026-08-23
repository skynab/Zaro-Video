#pragma once

#include <string>
#include <vector>

#include "zaro/core/Error.h"
#include "zaro/core/model/Ids.h"
#include "zaro/core/render/FrameSource.h"
#include "zaro/core/time/RationalTime.h"

namespace zaro::render {

struct ReframeOptions {
    /// How much camera movement counts as intended, in seconds.
    ///
    /// Longer than the stabiliser's window on purpose: a reframe is a shot
    /// being re-composed, and a frame that hurried after every gesture would
    /// look like a camera operator who had had too much coffee.
    double smoothingSeconds{0.8};
};

struct ReframeResult {
    /// Where to move the picture, per frame, in output pixels.
    std::vector<double> x;
    std::vector<double> y;
    /// How much to scale it so the new frame is filled with no empty edges.
    double scale{1.0};
    int measured{0};
    /// Set when nothing in the picture stood out, in which case the answer is
    /// the middle -- said rather than presented as a decision.
    std::string reason;
};

/// Choose where to put a landscape shot inside a portrait frame, or the other
/// way round, following whatever is interesting in it.
///
/// **Scale to cover, then choose an offset.** Filling the new frame is not
/// negotiable -- an empty edge is a mistake, not a look -- so the only free
/// parameter is where the window sits along whichever axis has slack.
///
/// **Interest is edge energy, not a face detector.** The sum of luminance
/// gradients in each column and row: where the detail is. A face detector
/// answers a narrower question better and a wider question worse -- a
/// landscape, a car, a hand, a graphic -- and it is a model to ship, keep
/// current and explain. Edges are what almost every shot's subject has more of
/// than its background.
///
/// **Smoothed like a camera move, not snapped per frame.** The window's path
/// is smoothed over most of a second, so what comes out is a slow reframe
/// rather than the window twitching between two equally interesting things.
///
/// **A flat shot centres, and says so.** Where nothing stands out, the middle
/// is the only defensible answer, and reporting that beats presenting it as a
/// decision somebody might trust.
[[nodiscard]] Result<ReframeResult> autoReframe(FrameSource& source, model::MediaRefId media,
                                                const std::vector<time::RationalTime>& sourceTimes,
                                                std::int32_t targetWidth, std::int32_t targetHeight,
                                                const ReframeOptions& options = {});

}  // namespace zaro::render
