#include "zaro/platform/qrhi/GpuRenderGraph.h"

#include "zaro/core/render/Grade.h"

namespace zaro::platform::qrhi {

Status GpuRenderGraph::drawClips(const model::Sequence& sequence, const time::RationalTime& at) {
    lastClipCount_ = 0;

    // Bottom-up. Index 0 is V1, the lowest track, and each later track
    // composites over what is already there.
    for (const model::Track& track : sequence.videoTracks()) {
        if (track.isMuted()) {
            continue;
        }
        // A transition shows both of its clips at once, reading each into the
        // handles beyond the cut. Mirrors render::RenderGraph exactly; the two
        // traversals have to agree or the preview and the export disagree.
        if (const model::Transition* transition = track.transitionAt(at)) {
            const model::Clip* outgoing = track.find(transition->from);
            const model::Clip* incoming = track.find(transition->to);
            if (outgoing != nullptr && incoming != nullptr) {
                const double progress = transition->progressAt(at);

                if (outgoing->enabled) {
                    if (auto frame = provider_->sourceFrameFor(outgoing->source,
                                                               outgoing->sourceTimeAt(at))) {
                        if (compositor_->drawSource(
                                **frame, outgoing->transformAt(at),
                                render::gradeConstantsFor(outgoing->colorAt(at)), outgoing->blend,
                                &curves_.tableFor(outgoing->id.value(), outgoing->curves,
                                                  transfer_))) {
                            ++lastClipCount_;
                        }
                    }
                }
                if (incoming->enabled) {
                    if (auto frame = provider_->sourceFrameFor(incoming->source,
                                                               incoming->sourceTimeAt(at))) {
                        model::Transform fading = incoming->transformAt(at);
                        fading.opacity *= progress;
                        if (compositor_->drawSource(
                                **frame, fading, render::gradeConstantsFor(incoming->colorAt(at)),
                                incoming->blend)) {
                            ++lastClipCount_;
                        }
                    }
                }
                continue;
            }
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
        if (Status drawn =
                compositor_->drawSource(**frame, clip->transformAt(at),
                                        render::gradeConstantsFor(clip->colorAt(at)), clip->blend);
            !drawn) {
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

Status GpuRenderGraph::compositeOn(::QRhiCommandBuffer* commandBuffer,
                                   const model::Sequence& sequence, const time::RationalTime& at) {
    if (Status begun =
            compositor_->beginFrameOn(commandBuffer, sequence.width(), sequence.height());
        !begun) {
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
