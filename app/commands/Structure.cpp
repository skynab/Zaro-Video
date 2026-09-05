// Operations that change the shape of the cut rather than the look of it.

#include "Structure.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/model/Graphic.h"
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

Result<edit::ClipRef> addTitle(const Context& context, const TitlePreset& preset,
                               const time::RationalTime& duration) {
    const model::Sequence* sequence = context.sequence();
    if (sequence == nullptr) {
        return Error{ErrorCode::InvalidData, "there is no sequence to put a title in"};
    }
    if (duration.toSecondsDouble() <= 0.0) {
        return Error{ErrorCode::InvalidData, "a title needs a length"};
    }

    // The one catalogue, so that asking for a caption here and dragging one out
    // of the Titles tab land the same clip. It is also what makes the preset
    // scale with the frame: the rasteriser reads `pointSize` as pixels, so a
    // fixed default would be illegible on a 4K cut and enormous on a thumbnail.
    const model::Graphic graphic = graphicFor(preset, sequence->width(), sequence->height());

    const time::Rational& rate = sequence->frameRate();
    const time::TimeRange range{context.position.rescaledTo(rate), duration.rescaledTo(rate)};

    // One undo step for the whole thing, including the row it may have to make:
    // the row is part of putting the title down rather than a separate thing
    // anybody asked for.
    const edit::CommandStack::Group step{context.commands()};

    model::TrackId target = context.track;
    const model::Track* picked = sequence->findTrack(target);
    if (picked == nullptr || picked->kind() != model::TrackKind::Video) {
        target = sequence->videoTracks().empty() ? model::TrackId{}
                                                 : sequence->videoTracks().front().id();
        picked = target.isValid() ? sequence->findTrack(target) : nullptr;
    }
    const bool needsRow =
        picked == nullptr || picked->isLocked() || !picked->clipsIn(range).empty();
    if (needsRow) {
        const std::size_t count = sequence->videoTracks().size();
        auto track = edit::makeAddTrack(context.project(), sequence->id(), model::TrackKind::Video,
                                        "V" + std::to_string(count + 1));
        if (!track) {
            return track.error();
        }
        context.commands().execute(context.project(), std::move(*track));
        // The command replaced the sequence wholesale, so nothing read before
        // this line may be used after it.
        sequence = context.sequence();
        if (sequence == nullptr || sequence->videoTracks().empty()) {
            return Error{ErrorCode::Internal, "the row for the title did not appear"};
        }
        target = sequence->videoTracks().back().id();
    }

    auto built = edit::makeAddGraphic(context.project(), {sequence->id(), target}, graphic, range);
    if (!built) {
        return built.error();
    }
    context.commands().execute(context.project(), std::move(*built));
    context.commands().breakMerge();

    sequence = context.sequence();
    const model::Track* landed = sequence != nullptr ? sequence->findTrack(target) : nullptr;
    if (landed == nullptr) {
        return Error{ErrorCode::Internal, "the title's row went missing"};
    }
    for (const model::Clip& clip : landed->clips()) {
        if (clip.start() == range.start() && clip.graphic.kind == model::GraphicKind::Text) {
            return edit::ClipRef{target, clip.id};
        }
    }
    return Error{ErrorCode::Internal, "the title did not land anywhere"};
}

Status animateTitle(const Context& context, TitleMotion motion) {
    const model::Sequence* sequence = context.sequence();
    const model::Clip* clip = context.selectedClip();
    if (sequence == nullptr || clip == nullptr) {
        return Error{ErrorCode::InvalidData, "select a title first"};
    }
    if (clip->graphic.kind != model::GraphicKind::Text) {
        return Error{ErrorCode::Unsupported, "that clip is not a title"};
    }

    // Keyframes are written in the clip's own source time, which for a graphic
    // runs alongside its time on the timeline: `makeAddGraphic` gives it a
    // source range identical to its timeline range for exactly this reason.
    const time::TimeRange range = clip->sourceRange;
    const time::Rational& rate = range.duration().rate();
    const double seconds = std::min(0.5, range.duration().toSecondsDouble() / 3.0);
    if (seconds <= 0.0) {
        return Error{ErrorCode::InvalidData, "that title is too short to animate"};
    }
    const time::RationalTime span =
        time::RationalTime::fromSeconds(time::Rational::approximate(seconds), rate);
    const time::RationalTime first = range.start();
    const time::RationalTime last = range.endExclusive() - time::RationalTime{1, rate};

    // One undo step for the pair, or the four: a move is one decision, and
    // taking half of it back leaves a title that fades in from nothing and
    // never arrives.
    const edit::CommandStack::Group step{context.commands()};
    const edit::EditTarget target{sequence->id(), context.track};

    const auto key = [&](model::Param param, const time::RationalTime& when, double value) {
        auto built = edit::makeSetKeyframe(context.project(), target, clip->id, param, when, value);
        if (built) {
            context.commands().execute(context.project(), std::move(*built));
        }
        return static_cast<bool>(built);
    };

    switch (motion) {
        case TitleMotion::FadeIn:
            if (!key(model::Param::Opacity, first, 0.0) ||
                !key(model::Param::Opacity, first + span, 1.0)) {
                return Error{ErrorCode::Internal, "the fade could not be written"};
            }
            break;
        case TitleMotion::FadeOut:
            if (!key(model::Param::Opacity, last - span, 1.0) ||
                !key(model::Param::Opacity, last, 0.0)) {
                return Error{ErrorCode::Internal, "the fade could not be written"};
            }
            break;
        case TitleMotion::Typewriter: {
            // Over most of the clip rather than over half a second: a
            // typewriter is the reading, not an entrance. The last part is
            // left alone so the finished line holds before the clip ends --
            // text that completes on its final frame reads as text nobody
            // finished writing.
            const time::RationalTime typed = time::RationalTime::fromSeconds(
                time::Rational::approximate(range.duration().toSecondsDouble() * 0.7), rate);
            if (!key(model::Param::TextReveal, first, 0.0) ||
                !key(model::Param::TextReveal, first + typed, 1.0)) {
                return Error{ErrorCode::Internal, "the typewriter could not be written"};
            }
            break;
        }
        case TitleMotion::SlideOn: {
            // A short travel and a fade together, which is what a lower third
            // does: a slide with no fade reads as a mistake at the frame edge,
            // and the distance is a fraction of the frame so it looks the same
            // whatever the sequence is.
            const double travel = -static_cast<double>(sequence->width()) * 0.12;
            if (!key(model::Param::PositionX, first, travel) ||
                !key(model::Param::PositionX, first + span, 0.0) ||
                !key(model::Param::Opacity, first, 0.0) ||
                !key(model::Param::Opacity, first + span, 1.0)) {
                return Error{ErrorCode::Internal, "the slide could not be written"};
            }
            break;
        }
    }
    context.commands().breakMerge();
    return {};
}

}  // namespace zaro::app::commands
