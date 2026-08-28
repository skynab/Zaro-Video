// Operations that change the shape of the cut rather than the look of it.

#include "Structure.h"

#include <cstdint>

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

Result<model::ClipId> pinToClipBelow(const Context& context) {
    const model::Sequence* sequence = context.sequence();
    if (sequence == nullptr || !context.clip.isValid()) {
        return Error{ErrorCode::InvalidData, "select the clip to pin first"};
    }
    const model::Clip* found = nullptr;
    for (const model::Track& track : sequence->videoTracks()) {
        if (track.id() == context.track) {
            break;  // tracks are listed bottom-up, so this is where "below" ends
        }
        if (!sequence->isAudible(track)) {
            continue;
        }
        if (const model::Clip* candidate = track.clipAt(context.position)) {
            found = candidate;
        }
    }
    if (found == nullptr) {
        return Error{ErrorCode::InvalidData, "there is nothing under this clip to pin it to"};
    }
    return pinTo(context, found->id);
}

Status setDelivery(const Context& context, const model::Sequence::Output& output) {
    const model::Sequence* sequence = context.sequence();
    if (sequence == nullptr) {
        return Error{ErrorCode::InvalidData, "there is no sequence to deliver"};
    }
    auto built = edit::makeSetSequenceOutput(context.project(), sequence->id(), output);
    if (!built) {
        return built.error();
    }
    context.commands().execute(context.project(), std::move(*built));
    return {};
}

Status replaceSelectedSource(const Context& context, model::MediaRefId media) {
    const model::Sequence* sequence = context.sequence();
    if (sequence == nullptr || !context.clip.isValid()) {
        return Error{ErrorCode::InvalidData, "select the clip to replace first"};
    }
    auto built = edit::makeReplaceSource(context.project(), context.target(), context.clip, media);
    if (!built) {
        return built.error();
    }
    context.commands().execute(context.project(), std::move(*built));
    return {};
}

Result<model::SubclipId> makeSubclip(const Context& context, model::MediaRefId source,
                                     const time::TimeRange& range) {
    const model::MediaRef* ref = context.project().findMedia(source);
    if (ref == nullptr) {
        return Error{ErrorCode::NotFound, "no such media in this project"};
    }
    model::Subclip subclip;
    subclip.id = context.project().ids().next<model::SubclipTag>();
    subclip.source = ref->id;
    subclip.range = range;
    std::size_t existing = 0;
    for (const model::Subclip& other : context.project().subclips()) {
        existing += other.source == ref->id ? 1U : 0U;
    }
    subclip.name = ref->name + " [" + std::to_string(existing + 1) + "]";
    const model::SubclipId id = subclip.id;
    context.project().addSubclip(std::move(subclip));
    return id;
}

Result<MatchedFrame> frameToMatch(const Context& context) {
    const model::Clip* clip = context.selectedClip();
    if (clip == nullptr ||
        !clip->timelineRange.contains(context.position.rescaledTo(clip->start().rate()))) {
        return Error{ErrorCode::InvalidData, "put the playhead over the selected clip first"};
    }
    const model::MediaRef* ref = context.project().findMedia(clip->activeSource());
    if (ref == nullptr) {
        return Error{ErrorCode::InvalidData, "this clip is generated: there is no frame to match"};
    }
    return MatchedFrame{ref->id, clip->activeSourceTimeAt(context.position)};
}

}  // namespace zaro::app::commands
