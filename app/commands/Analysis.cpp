// Operations that look at the frames and write what they find.
//
// Each of these decodes and composites the clip it is given, measures something
// about the pictures that came out, and turns the answer into keyframes through
// the command stack. They were methods on PreviewWindow because that is where
// the project, the media and the render cache are kept; none of them is about
// the window, and none of them opens a dialog or repaints anything. What the
// window does after one returns is its own business -- see afterEdit.

#include "Analysis.h"

#include <algorithm>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/render/PathRaster.h"
#include "zaro/core/render/Reframe.h"
#include "zaro/core/render/RenderGraph.h"
#include "zaro/core/render/ShotMatch.h"
#include "zaro/core/render/Stabilise.h"
#include "zaro/core/render/Tracker.h"

namespace zaro::app::commands {
namespace {

/// A CPU graph over the project, set up the way the preview sets one up.
///
/// The same four lines appeared at the top of every operation here, and getting
/// one of them wrong -- forgetting the cache, say -- does not fail, it just
/// quietly composites everything again.
render::RenderGraph graphFor(const Context& context) {
    render::RenderGraph graph{*context.media};
    graph.setProject(&context.project());
    graph.setTextRasterizer(context.text);
    graph.setRenderCache(context.cache);
    return graph;
}

}  // namespace

/// Match the selected clip to the frame being held as the reference.
///
/// Returns the match so a caller can say what happened. Nothing is applied
/// when the two shots are too unalike: an automatic grade that is confidently
/// wrong is worse than none, and the person looking at both frames is better
/// placed to decide than a distance measure is.
Result<render::ShotMatch> matchToReference(const Context& context,
                                           const time::RationalTime& reference) {
    const model::Sequence* sequence = context.sequence();
    const model::Clip* clip = context.selectedClip();
    if (clip == nullptr || context.media == nullptr) {
        return Error{ErrorCode::InvalidData, "select the clip to match first"};
    }

    // Both frames composited the same way, through the CPU graph the
    // comparison already uses -- so what is matched is what is on screen.
    render::RenderGraph graph = graphFor(context);

    render::RgbaImage referenceFrame;
    if (Status status = graph.compositeInto(*sequence, reference, referenceFrame); !status) {
        return status.error();
    }
    render::RgbaImage current;
    if (Status status = graph.compositeInto(*sequence, context.position, current); !status) {
        return status.error();
    }

    auto match = render::matchShot(referenceFrame, current);
    if (!match) {
        return match.error();
    }
    if (!match->usable) {
        return match;
    }

    auto built =
        edit::makeSetWheels(context.project(), context.target(), context.clip, match->wheels);
    if (!built) {
        return built.error();
    }
    context.commands().execute(context.project(), std::move(*built));
    context.commands().breakMerge();
    return match;
}

/// Follow the selected clip's mask through the rest of the clip.
///
/// **Frame to frame, not against the first frame.** A reference frame does
/// not drift, but it also stops matching the moment the thing turns, moves
/// under a different light, or is partly covered -- which is most shots
/// worth tracking. Frame to frame follows all of that and accumulates a
/// little error instead, which is the trade every tracker makes and the one
/// people can correct by hand afterwards.
///
/// **On the composited picture, not on the decoded source.** The mask lives
/// in output coordinates over whatever is on screen, so what it has to
/// follow is what is on screen.
Result<MaskTrack> trackMaskForward(const Context& context, const Progress& tell) {
    const model::Sequence* sequence = context.sequence();
    const model::Clip* clip = context.selectedClip();
    if (clip == nullptr || context.media == nullptr) {
        return Error{ErrorCode::InvalidData, "select the clip whose mask should be tracked"};
    }
    if (!clip->mask.isSet()) {
        return Error{ErrorCode::InvalidData, "that clip has no mask to track"};
    }
    const time::RationalTime from = context.position;
    if (from < clip->start() || from >= clip->endExclusive()) {
        return Error{ErrorCode::InvalidData, "put the playhead over the clip first"};
    }

    const render::MaskBounds bounds = render::maskBounds(clip->maskAt(from));
    if (bounds.isEmpty()) {
        return Error{ErrorCode::InvalidData, "that mask has no area to track"};
    }

    render::RenderGraph graph = graphFor(context);

    const auto rate = sequence->frameRate();
    const auto step = time::RationalTime{1, rate};
    render::RgbaImage previous;
    if (Status status = graph.compositeInto(*sequence, from, previous); !status) {
        return status.error();
    }

    // The search window scales with the frame rather than being a fixed
    // number of pixels: twenty pixels is a big move at 720p and a twitch at
    // 4K, and what somebody means by "it moves about this fast" is the
    // former. Capped, because the cost is the square of it and a window
    // wide enough to cover a whipping pan is also wide enough to find the
    // wrong lamppost.
    render::PatchWindow window;
    window.search = std::clamp(static_cast<double>(sequence->width()) * 0.02, 8.0, 48.0);

    model::Curve xs;
    model::Curve ys;
    double dx = clip->parameterAt(model::Param::MaskX, from);
    double dy = clip->parameterAt(model::Param::MaskY, from);
    // Both curves start with where the mask already is, so the frames
    // before the track are not dragged along by the first keyframe.
    xs.set(model::Keyframe{clip->sourceTimeAt(from), dx, model::Interpolation::Linear, {}, {}});
    ys.set(model::Keyframe{clip->sourceTimeAt(from), dy, model::Interpolation::Linear, {}, {}});

    MaskTrack result;
    result.confidence = 1.0;
    const std::int64_t totalFrames = (clip->endExclusive() - from).rescaledTo(rate).frames();
    std::int64_t doneFrames = 0;
    for (time::RationalTime at = from + step; at < clip->endExclusive(); at = at + step) {
        ++doneFrames;
        if (tell && !tell(doneFrames, totalFrames)) {
            // Stopped by hand. What was tracked up to here is kept: the
            // keyframes so far are real work, and throwing them away because
            // somebody stopped a long track early is the opposite of what
            // stopping it was for.
            result.stopped = "stopped here";
            break;
        }
        render::RgbaImage current;
        if (Status status = graph.compositeInto(*sequence, at, current); !status) {
            return status.error();
        }
        window.centreX = (static_cast<double>(sequence->width()) / 2.0) + bounds.centreX() + dx;
        window.centreY = (static_cast<double>(sequence->height()) / 2.0) + bounds.centreY() + dy;
        window.halfWidth = std::max(8.0, bounds.width() / 2.0);
        window.halfHeight = std::max(8.0, bounds.height() / 2.0);

        const render::PatchTrack moved = render::trackPatch(previous, current, window);
        if (!moved.usable) {
            result.stopped = moved.reason;
            break;
        }
        dx += moved.dx;
        dy += moved.dy;
        result.confidence = std::min(result.confidence, moved.confidence);
        ++result.frames;
        const time::RationalTime when = clip->sourceTimeAt(at);
        xs.set(model::Keyframe{when, dx, model::Interpolation::Linear, {}, {}});
        ys.set(model::Keyframe{when, dy, model::Interpolation::Linear, {}, {}});
        previous = std::move(current);
    }

    if (result.frames == 0) {
        return Error{ErrorCode::InvalidData, result.stopped.empty()
                                                 ? "there is nothing after this frame to track into"
                                                 : result.stopped};
    }

    auto built = edit::makeTrackMask(context.project(), context.target(), context.clip, xs, ys);
    if (!built) {
        return built.error();
    }
    context.commands().execute(context.project(), std::move(*built));
    context.commands().breakMerge();
    return result;
}

/// Hold the selected clip still.
///
/// **On the clip's own frames, not on the composite.** What is being
/// measured is how the camera moved, and the composite already has this
/// clip's transform applied to it -- including the correction being
/// computed, which would make the analysis chase its own tail. It also has
/// whatever is layered over the clip in it, which moved for reasons of its
/// own.
Result<render::StabiliseResult> stabiliseClip(const Context& context, const Progress& tell) {
    const model::Sequence* sequence = context.sequence();
    const model::Clip* clip = context.selectedClip();
    if (clip == nullptr || context.media == nullptr) {
        return Error{ErrorCode::InvalidData, "select the clip to stabilise"};
    }
    if (clip->graphic.kind != model::GraphicKind::None || clip->nested.isValid() ||
        !clip->activeSource().isValid()) {
        return Error{ErrorCode::InvalidData,
                     "there is nothing to stabilise: this clip is generated, not filmed"};
    }

    const auto step = time::RationalTime{1, sequence->frameRate()};
    std::vector<time::RationalTime> times;
    std::vector<time::RationalTime> timeline;
    for (time::RationalTime at = clip->start(); at < clip->endExclusive(); at = at + step) {
        timeline.push_back(at);
        times.push_back(clip->activeSourceTimeAt(at));
    }

    auto analysed = render::stabilise(*context.media, clip->activeSource(), times, {}, tell);
    if (!analysed) {
        return analysed;
    }

    model::Curve xs;
    model::Curve ys;
    for (std::size_t i = 0; i < analysed->x.size() && i < timeline.size(); ++i) {
        // Keyframes at the clip's own source times, like every other curve
        // in the project: a stabilised clip that is then trimmed or moved
        // keeps its correction glued to the frames it was measured from.
        const time::RationalTime when = clip->sourceTimeAt(timeline[i]);
        xs.set(model::Keyframe{when, analysed->x[i], model::Interpolation::Linear, {}, {}});
        ys.set(model::Keyframe{when, analysed->y[i], model::Interpolation::Linear, {}, {}});
    }
    if (xs.empty()) {
        return Error{ErrorCode::InvalidData, "there is not enough of this clip to stabilise"};
    }

    auto built = edit::makeStabilise(context.project(), context.target(), context.clip, xs, ys,
                                     analysed->zoom);
    if (!built) {
        return built.error();
    }
    context.commands().execute(context.project(), std::move(*built));
    context.commands().breakMerge();
    return analysed;
}

/// Recompose the selected clip to fill the sequence's frame.
///
/// **On the clip's own frames**, like the stabiliser and for the same
/// reason: the composite already has this clip's transform on it, and the
/// transform is what is being decided.
Result<render::ReframeResult> reframeClip(const Context& context) {
    const model::Sequence* sequence = context.sequence();
    const model::Clip* clip = context.selectedClip();
    if (clip == nullptr || context.media == nullptr) {
        return Error{ErrorCode::InvalidData, "select the clip to reframe"};
    }
    if (clip->graphic.kind != model::GraphicKind::None || clip->nested.isValid() ||
        !clip->activeSource().isValid()) {
        return Error{ErrorCode::InvalidData,
                     "there is nothing to reframe: this clip is generated, not filmed"};
    }

    const auto step = time::RationalTime{1, sequence->frameRate()};
    std::vector<time::RationalTime> times;
    std::vector<time::RationalTime> timeline;
    for (time::RationalTime at = clip->start(); at < clip->endExclusive(); at = at + step) {
        timeline.push_back(at);
        times.push_back(clip->activeSourceTimeAt(at));
    }

    auto framed = render::autoReframe(*context.media, clip->activeSource(), times,
                                      sequence->width(), sequence->height());
    if (!framed) {
        return framed;
    }

    model::Curve xs;
    model::Curve ys;
    for (std::size_t i = 0; i < framed->x.size() && i < timeline.size(); ++i) {
        const time::RationalTime when = clip->sourceTimeAt(timeline[i]);
        xs.set(model::Keyframe{when, framed->x[i], model::Interpolation::Linear, {}, {}});
        ys.set(model::Keyframe{when, framed->y[i], model::Interpolation::Linear, {}, {}});
    }
    if (xs.empty()) {
        return Error{ErrorCode::InvalidData, "there is not enough of this clip to reframe"};
    }

    auto built =
        edit::makeReframe(context.project(), context.target(), context.clip, xs, ys, framed->scale);
    if (!built) {
        return built.error();
    }
    context.commands().execute(context.project(), std::move(*built));
    context.commands().breakMerge();
    return framed;
}

}  // namespace zaro::app::commands
