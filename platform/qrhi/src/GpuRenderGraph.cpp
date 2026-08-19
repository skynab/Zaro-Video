#include "zaro/platform/qrhi/GpuRenderGraph.h"

#include "zaro/core/render/Grade.h"

namespace zaro::platform::qrhi {

/// Draw one clip, with everything that applies to it.
///
/// A single place, for the same reason the CPU graph has one: the three call
/// sites below each used to compute the grade for themselves and they drifted
/// apart, leaving one half of a transition without its tone curves.
bool GpuRenderGraph::drawClip(const model::Clip& clip, const media::VideoFrame& frame,
                              const model::Transform& transform, const time::RationalTime& at) {
    const render::SecondaryConstants secondary =
        render::secondaryConstantsFor(clip.secondary, transfer_);
    return compositor_
        ->drawSource(frame, transform, render::gradeConstantsFor(clip.colorAt(at)), clip.blend,
                     &curves_.tableFor(clip.id.value(), clip.curves, transfer_), &secondary)
        .ok();
}

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
                        if (drawClip(*outgoing, **frame, outgoing->transformAt(at), at)) {
                            ++lastClipCount_;
                        }
                    }
                }
                if (incoming->enabled) {
                    if (auto frame = provider_->sourceFrameFor(incoming->source,
                                                               incoming->sourceTimeAt(at))) {
                        model::Transform fading = incoming->transformAt(at);
                        fading.opacity *= progress;
                        if (drawClip(*incoming, **frame, fading, at)) {
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
        if (!drawClip(*clip, **frame, clip->transformAt(at), at)) {
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
