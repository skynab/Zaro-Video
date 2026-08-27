// Operations that change the shape of the cut rather than the look of it.

#include "Structure.h"

#include "zaro/core/edit/Operations.h"
#include "zaro/core/render/SceneDetect.h"

namespace zaro::app::commands {

/// Pin the selected clip to one on a lower track, or to nothing.
Result<model::ClipId> pinTo(const Context& context, model::ClipId host) {
    const model::Sequence* sequence = context.sequence();
    if (sequence == nullptr || !context.clip.isValid()) {
        return Error{ErrorCode::InvalidData, "select the clip to pin first"};
    }
    auto built =
        edit::makePinTo(context.project(), {sequence->id(), context.track}, context.clip, host);
    if (!built) {
        return built.error();
    }
    context.commands().execute(context.project(), std::move(*built));
    return host;
}

/// Cut the selected clip where the picture changes.
///
/// Returns how many cuts were made, so the self-test can say what happened
/// without a dialog. Zero is a perfectly good answer: a single continuous
/// take has no scene changes in it, and reporting one would be worse than
/// reporting none.
std::int32_t detectScenes(const Context& context, const Progress& tell) {
    const model::Sequence* sequence = context.sequence();
    const model::Clip* clip = context.selectedClip();
    if (clip == nullptr || context.media == nullptr || clip->nested.isValid() ||
        clip->graphic.isSet()) {
        return 0;
    }

    const time::Rational rate = sequence->frameRate();
    const std::int64_t first = clip->start().rescaledTo(rate).frames();
    const std::int64_t count = clip->timelineRange.duration().rescaledTo(rate).frames();
    if (count <= 1) {
        return 0;
    }

    render::SceneDetectOptions options;
    // Half a second, at whatever rate this sequence runs. Expressed in time
    // rather than frames so the same setting means the same thing on a
    // 24fps cut and a 60fps one.
    options.minimumShot = time::RationalTime::fromSeconds(time::Rational{1, 2}, rate);

    render::SceneDetector detector{options};
    for (std::int64_t i = 0; i < count; ++i) {
        if (tell && !tell(i, count)) {
            return 0;
        }
        const time::RationalTime at{first + i, rate};
        auto image = context.media->imageFor(clip->activeSource(), clip->activeSourceTimeAt(at));
        if (!image) {
            // A frame that will not decode is a gap in the evidence, not a
            // scene change. Skipped, and the frame before it stays the one
            // the next is compared against.
            continue;
        }
        detector.push(**image, at);
    }
    detector.flush();

    std::vector<time::RationalTime> points;
    points.reserve(detector.cuts().size());
    for (const render::SceneCut& cut : detector.cuts()) {
        points.push_back(cut.at);
    }
    if (points.empty()) {
        return 0;
    }

    auto built = edit::makeRazorAt(context.project(), {sequence->id(), context.track}, points);
    if (!built) {
        return 0;
    }
    context.commands().execute(context.project(), std::move(*built));
    return static_cast<std::int32_t>(points.size());
}

}  // namespace zaro::app::commands
