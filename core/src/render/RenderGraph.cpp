#include "zaro/core/render/RenderGraph.h"

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
        drawTransformed(**image, out, clip->transform, clip->blend);
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
