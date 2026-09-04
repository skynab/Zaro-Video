#include "zaro/platform/qrhi/GpuRenderGraph.h"

#include "zaro/core/render/Grade.h"
#include "zaro/core/render/ShapeRaster.h"
#include "zaro/core/render/TextRasterizer.h"

namespace zaro::platform::qrhi {

using model::pinnedTransformAt;

/// Draw one clip, with everything that applies to it.
///
/// A single place, for the same reason the CPU graph has one: the three call
/// sites below each used to compute the grade for themselves and they drifted
/// apart, leaving one half of a transition without its tone curves.
bool GpuRenderGraph::drawClipImage(const model::Clip& clip, const render::RgbaImage& image,
                                   const model::Transform& transform, const time::RationalTime& at,
                                   const model::Mask* wipe) {
    const render::SecondaryConstants secondary =
        render::secondaryConstantsFor(clip.secondary, transfer_);
    const render::LutTable* lut =
        clip.lut.isSet() ? luts_.tableFor(clip.lut.path, transfer_) : nullptr;
    const render::KeyerConstants keyer = render::keyerConstantsFor(clip.keyer, transfer_);
    // Sampled, not read off the clip: a tracked mask moves frame by frame, and
    // the preview showing it where it was drawn while the export shows it where
    // it was tracked to is the worst of both.
    const model::Mask mask = clip.maskAt(at);
    return compositor_
        ->draw(
            image, transform, clip.blend, render::gradeConstantsFor(clip.colorAt(at), clip.wheels),
            &curves_.tableFor(clip.id.value(), clip.curves, transfer_), &secondary, lut,
            static_cast<float>(clip.lut.amount), mask.isSet() ? &mask : nullptr,
            keyer.isActive() ? &keyer : nullptr, clip.vignette.isSet() ? &clip.vignette : nullptr,
            wipe, &colorCurves_.tableFor(clip.id.value(), clip.colorCurves))
        .ok();
}

bool GpuRenderGraph::drawClip(const model::Clip& clip, const media::VideoFrame& frame,
                              const model::Transform& transform, const time::RationalTime& at,
                              const model::Mask* wipe) {
    const render::SecondaryConstants secondary =
        render::secondaryConstantsFor(clip.secondary, transfer_);
    const render::LutTable* lut =
        clip.lut.isSet() ? luts_.tableFor(clip.lut.path, transfer_) : nullptr;
    const render::KeyerConstants keyer = render::keyerConstantsFor(clip.keyer, transfer_);
    const model::Mask mask = clip.maskAt(at);
    return compositor_
        ->drawSource(frame, transform, render::gradeConstantsFor(clip.colorAt(at), clip.wheels),
                     clip.blend, &curves_.tableFor(clip.id.value(), clip.curves, transfer_),
                     &secondary, lut, static_cast<float>(clip.lut.amount),
                     mask.isSet() ? &mask : nullptr, keyer.isActive() ? &keyer : nullptr,
                     clip.vignette.isSet() ? &clip.vignette : nullptr, wipe,
                     &colorCurves_.tableFor(clip.id.value(), clip.colorCurves))
        .ok();
}

render::RenderGraph* GpuRenderGraph::cpuGraph() {
    if (nestedSource_ == nullptr) {
        return nullptr;
    }
    if (nested_ == nullptr) {
        nested_ = std::make_unique<render::RenderGraph>(*nestedSource_);
        nested_->setProject(project_);
        nested_->setTextRasterizer(text_);
        nested_->setRenderCache(cache_);
    }
    return nested_.get();
}

bool GpuRenderGraph::drawTransitionSide(const model::Clip& clip, const model::Sequence& sequence,
                                        const model::Transform& transform,
                                        const time::RationalTime& at, render::RgbaImage& scratch,
                                        const model::Mask* wipe) {
    if (clip.graphic.isSet()) {
        if (scratch.width() != sequence.width() || scratch.height() != sequence.height()) {
            scratch = render::RgbaImage{sequence.width(), sequence.height()};
        }
        if (clip.graphic.kind == model::GraphicKind::Text) {
            if (!render::drawText(clip.graphic, text_, scratch,
                                  clip.parameterAt(model::Param::TextReveal, at))) {
                // No font engine, or it failed. A missing title is a visible,
                // diagnosable gap; a failed render is a stalled edit.
                return false;
            }
        } else {
            render::drawShape(clip.graphic, scratch);
        }
        return drawClipImage(clip, scratch, transform, at, wipe);
    }
    auto frame = provider_->sourceFrameFor(clip.activeSource(), clip.activeSourceTimeAt(at));
    if (!frame) {
        return false;
    }
    return drawClip(clip, **frame, transform, at, wipe);
}

bool GpuRenderGraph::needsCpuFallback(const model::Sequence& sequence,
                                      const time::RationalTime& at) {
    for (const model::Track& track : sequence.videoTracks()) {
        if (!sequence.isAudible(track)) {
            continue;
        }
        const model::Clip* clip = track.clipAt(at);
        if (clip == nullptr || !clip->enabled) {
            continue;
        }
        // An adjustment layer needs the accumulated frame read back; an effect
        // needs a pixel's neighbours, which this compositor's single sampling
        // pass cannot reach. Both are things the CPU graph already does
        // correctly, and neither is on the path that delivers.
        // A path mask is a third: its coverage is a scanline fill of the whole
        // frame, not a formula a fragment shader can answer per pixel from a
        // handful of uniforms. Handing the GPU a coverage texture per clip per
        // frame is the fast path and is worth having; the CPU graph already
        // produces exactly the right answer, and Phase 5w's render cache is
        // what keeps that affordable in the meantime.
        if (clip->adjustment || model::anyActive(clip->effects) ||
            clip->mask.shape == model::MaskShape::Path) {
            return true;
        }
    }
    return false;
}

Status GpuRenderGraph::drawClips(const model::Sequence& sequence, const time::RationalTime& at) {
    lastClipCount_ = 0;

    // Two things this compositor cannot do in its one queued pass: an
    // adjustment layer, which needs the accumulated frame read back and
    // corrected, and an effect, which needs to read a pixel's neighbours rather
    // than the one the sampler returned. Rather than restructure into ping-pong
    // passes and a per-clip pre-pass for cases that are rare in a preview and
    // never on the path that delivers, the whole frame is composited on the CPU
    // and uploaded.
    //
    // The result is not merely close to the export: it is the same code. The
    // cost is a slow frame wherever an adjustment layer is, and that is a
    // trade worth stating rather than hiding.
    if (needsCpuFallback(sequence, at) && nestedSource_ != nullptr) {
        if (nested_ == nullptr) {
            nested_ = std::make_unique<render::RenderGraph>(*nestedSource_);
            nested_->setProject(project_);
            nested_->setTextRasterizer(text_);
            nested_->setRenderCache(cache_);
        }
        auto frame = nested_->composite(sequence, at);
        if (!frame) {
            return frame.error();
        }
        lastClipCount_ = nested_->lastClipCount();
        return compositor_->draw(*frame, model::Transform{}, model::BlendMode::Normal);
    }

    // The same curve the CPU path reads, from the same place: these two
    // traversals have to agree, and a display curve taken from different
    // sources is exactly the kind of disagreement that only shows up in an
    // export somebody has already signed off.
    transfer_ = sequence.output().transfer;

    // Bottom-up. Index 0 is V1, the lowest track, and each later track
    // composites over what is already there.
    for (const model::Track& track : sequence.videoTracks()) {
        if (!sequence.isAudible(track)) {
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

                if (outgoing->enabled &&
                    drawTransitionSide(*outgoing, sequence,
                                       pinnedTransformAt(sequence, *outgoing, at), at, generated_,
                                       nullptr)) {
                    ++lastClipCount_;
                }
                if (incoming->enabled) {
                    // The same function the CPU path calls, so the two cannot
                    // come to different answers about where a wipe's edge is.
                    const render::TransitionShape shape = render::transitionShapeFor(
                        *transition, progress, sequence.width(), sequence.height());
                    model::Transform moving = pinnedTransformAt(sequence, *incoming, at);
                    moving.opacity *= shape.opacity;
                    moving.positionX += shape.offsetX;
                    moving.positionY += shape.offsetY;
                    if (drawTransitionSide(*incoming, sequence, moving, at, generatedB_,
                                           shape.wipe.isSet() ? &shape.wipe : nullptr)) {
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

        if (clip->nested.isValid()) {
            if (nestedSource_ == nullptr || project_ == nullptr) {
                continue;
            }
            const model::Sequence* inner = project_->findSequence(clip->nested);
            if (inner == nullptr) {
                continue;
            }
            if (nested_ == nullptr) {
                nested_ = std::make_unique<render::RenderGraph>(*nestedSource_);
                nested_->setProject(project_);
                nested_->setTextRasterizer(text_);
                nested_->setRenderCache(cache_);
            }
            auto frame = nested_->composite(*inner, clip->sourceTimeAt(at));
            if (!frame) {
                continue;
            }
            if (drawClipImage(*clip, *frame, pinnedTransformAt(sequence, *clip, at), at)) {
                ++lastClipCount_;
            }
            continue;
        }

        if (clip->graphic.isSet()) {
            // Rasterised on the CPU and uploaded through the compositor's
            // existing RGBA path. A shader for shapes would be faster, and
            // would be a second implementation of the geometry to keep in step
            // with the first -- for a buffer that only changes when somebody
            // edits the shape.
            if (generated_.width() != sequence.width() ||
                generated_.height() != sequence.height()) {
                generated_ = render::RgbaImage{sequence.width(), sequence.height()};
            }
            if (clip->graphic.kind == model::GraphicKind::Text) {
                if (!render::drawText(clip->graphic, text_, generated_,
                                      clip->parameterAt(model::Param::TextReveal, at))) {
                    continue;
                }
            } else {
                render::drawShape(clip->graphic, generated_);
            }
            if (drawClipImage(*clip, generated_, pinnedTransformAt(sequence, *clip, at), at)) {
                ++lastClipCount_;
            }
            continue;
        }

        auto frame = provider_->sourceFrameFor(clip->activeSource(), clip->activeSourceTimeAt(at));
        if (!frame) {
            // One unreadable clip must not take the whole frame with it, the
            // same as on the CPU path.
            continue;
        }
        if (!drawClip(*clip, **frame, pinnedTransformAt(sequence, *clip, at), at)) {
            continue;
        }
        ++lastClipCount_;
    }
    // Captions over everything, the same as the CPU path. The two have to put
    // them in the same place, which is why the graphic is built by shared code
    // rather than by each of them.
    if (sequence.captions().isBurnedIn() && text_ != nullptr) {
        for (const model::Caption* caption : sequence.captions().at(at)) {
            if (generated_.width() != sequence.width() ||
                generated_.height() != sequence.height()) {
                generated_ = render::RgbaImage{sequence.width(), sequence.height()};
            }
            const model::Graphic graphic = render::captionGraphic(
                sequence.captions().style(), caption->text, sequence.width(), sequence.height());
            if (render::drawText(graphic, text_, generated_)) {
                (void)compositor_->draw(generated_, model::Transform{}, model::BlendMode::Normal);
            }
        }
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
