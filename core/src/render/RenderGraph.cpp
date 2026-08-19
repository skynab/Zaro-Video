#include "zaro/core/render/RenderGraph.h"

#include "zaro/core/render/Grade.h"

namespace zaro::render {

Status RenderGraph::compositeInto(const model::Sequence& sequence, const time::RationalTime& at,
                                  RgbaImage& out) {
    if (sequence.width() <= 0 || sequence.height() <= 0) {
        return Error{ErrorCode::InvalidData, "the sequence has no frame size"};
    }
    if (out.width() != sequence.width() || out.height() != sequence.height()) {
        out = RgbaImage{sequence.width(), sequence.height()};
    }
    // Transparent black, not opaque black: a sequence with nothing on it is
    // empty rather than black, and the distinction matters the moment anything
    // is exported with an alpha channel.
    out.clear();
    lastClipCount_ = 0;

    // Bottom-up. Index 0 is V1, the lowest track, and each later track
    // composites over what is already there.
    for (const model::Track& track : sequence.videoTracks()) {
        if (track.isMuted()) {
            continue;
        }
        // A transition shows both of its clips at once. The clips themselves
        // never overlap on the timeline, so this is the only place two clips
        // from one track contribute to the same frame.
        if (const model::Transition* transition = track.transitionAt(at)) {
            const model::Clip* outgoing = track.find(transition->from);
            const model::Clip* incoming = track.find(transition->to);
            if (outgoing != nullptr && incoming != nullptr) {
                const auto progress = transition->progressAt(at);

                // The outgoing clip is read past its out point and the incoming
                // one before its in point, both reaching into the handles
                // either side of the cut. sourceTimeAt extrapolates linearly,
                // which is exactly the mapping wanted here.
                if (outgoing->enabled) {
                    if (auto image =
                            source_->imageFor(outgoing->source, outgoing->sourceTimeAt(at))) {
                        drawTransformed(**image, out, outgoing->transformAt(at), outgoing->blend);
                        ++lastClipCount_;
                    }
                }
                if (incoming->enabled) {
                    if (auto image =
                            source_->imageFor(incoming->source, incoming->sourceTimeAt(at))) {
                        // Drawn over the outgoing clip at the dissolve's
                        // progress: with premultiplied `over` and an opaque
                        // source that gives out*(1-p) + in*p.
                        model::Transform fading = incoming->transformAt(at);
                        fading.opacity *= progress;
                        const GradeConstants grade = gradeConstantsFor(incoming->colorAt(at));
                        drawTransformed(**image, out, fading, incoming->blend,
                                        grade.isIdentity() ? nullptr : &grade);
                        ++lastClipCount_;
                    }
                }
                continue;
            }
        }

        const model::Clip* clip = track.clipAt(at);
        if (clip == nullptr || !clip->enabled) {
            continue;
        }

        auto image = source_->imageFor(clip->source, clip->sourceTimeAt(at));
        if (!image) {
            // One unreadable clip must not take the whole frame with it. A gap
            // where a clip should be is a visible, diagnosable problem; a failed
            // render is a stalled edit.
            continue;
        }
        const GradeConstants grade = gradeConstantsFor(clip->colorAt(at));
        drawTransformed(**image, out, clip->transformAt(at), clip->blend,
                        grade.isIdentity() ? nullptr : &grade);
        ++lastClipCount_;
    }
    return {};
}

Result<RgbaImage> RenderGraph::composite(const model::Sequence& sequence,
                                         const time::RationalTime& at) {
    RgbaImage out;
    if (Status status = compositeInto(sequence, at, out); !status) {
        return status.error();
    }
    return out;
}

}  // namespace zaro::render
