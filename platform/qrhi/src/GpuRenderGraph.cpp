#include "zaro/platform/qrhi/GpuRenderGraph.h"

namespace zaro::platform::qrhi {

Status GpuRenderGraph::drawClips(const model::Sequence& sequence, const time::RationalTime& at) {
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

        auto frame = provider_->sourceFrameFor(clip->source, clip->sourceTimeAt(at));
        if (!frame) {
            // One unreadable clip must not take the whole frame with it, the
            // same as on the CPU path.
            continue;
        }
        if (Status drawn = compositor_->drawSource(**frame, clip->transform, clip->blend); !drawn) {
            continue;
        }
        ++lastClipCount_;
    }
    return {};
}

Status GpuRenderGraph::composite(const model::Sequence& sequence, const time::RationalTime& at) {
    if (Status begun = compositor_->beginFrame(sequence.width(), sequence.height()); !begun) {
        return begun;
    }
    if (Status drawn = drawClips(sequence, at); !drawn) {
        return drawn;
    }
    return compositor_->endFrameOnGpu();
}

Status GpuRenderGraph::compositeInto(const model::Sequence& sequence, const time::RationalTime& at,
                                     render::RgbaImage& out) {
    if (Status begun = compositor_->beginFrame(sequence.width(), sequence.height()); !begun) {
        return begun;
    }
    if (Status drawn = drawClips(sequence, at); !drawn) {
        return drawn;
    }
    return compositor_->endFrame(out);
}

}  // namespace zaro::platform::qrhi
