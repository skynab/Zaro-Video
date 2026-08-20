#include "zaro/platform/qrhi/GpuRenderGraph.h"

#include "zaro/core/render/Grade.h"
#include "zaro/core/render/ShapeRaster.h"
#include "zaro/core/render/TextRasterizer.h"

namespace zaro::platform::qrhi {

/// Draw one clip, with everything that applies to it.
///
/// A single place, for the same reason the CPU graph has one: the three call
/// sites below each used to compute the grade for themselves and they drifted
/// apart, leaving one half of a transition without its tone curves.
bool GpuRenderGraph::drawClipImage(const model::Clip& clip, const render::RgbaImage& image,
                                   const model::Transform& transform,
                                   const time::RationalTime& at) {
    const render::SecondaryConstants secondary =
        render::secondaryConstantsFor(clip.secondary, transfer_);
    const render::LutTable* lut =
        clip.lut.isSet() ? luts_.tableFor(clip.lut.path, transfer_) : nullptr;
    return compositor_
        ->draw(image, transform, clip.blend, render::gradeConstantsFor(clip.colorAt(at)),
               &curves_.tableFor(clip.id.value(), clip.curves, transfer_), &secondary, lut,
               static_cast<float>(clip.lut.amount), clip.mask.isSet() ? &clip.mask : nullptr)
        .ok();
}

bool GpuRenderGraph::drawClip(const model::Clip& clip, const media::VideoFrame& frame,
                              const model::Transform& transform, const time::RationalTime& at) {
    const render::SecondaryConstants secondary =
        render::secondaryConstantsFor(clip.secondary, transfer_);
    const render::LutTable* lut =
        clip.lut.isSet() ? luts_.tableFor(clip.lut.path, transfer_) : nullptr;
    return compositor_
        ->drawSource(frame, transform, render::gradeConstantsFor(clip.colorAt(at)), clip.blend,
                     &curves_.tableFor(clip.id.value(), clip.curves, transfer_), &secondary, lut,
                     static_cast<float>(clip.lut.amount), clip.mask.isSet() ? &clip.mask : nullptr)
        .ok();
}

bool GpuRenderGraph::hasAdjustment(const model::Sequence& sequence, const time::RationalTime& at) {
    for (const model::Track& track : sequence.videoTracks()) {
        if (!sequence.isAudible(track)) {
            continue;
        }
        const model::Clip* clip = track.clipAt(at);
        if (clip != nullptr && clip->enabled && clip->adjustment) {
            return true;
        }
    }
    return false;
}

Status GpuRenderGraph::drawClips(const model::Sequence& sequence, const time::RationalTime& at) {
    lastClipCount_ = 0;

    // An adjustment layer changes what has already been composited, and this
    // compositor queues every draw into one pass -- so there is no point at
    // which the accumulated frame can be read back and corrected. Rather than
    // restructure that into ping-pong passes for a case that is rare in a
    // preview and never on the path that delivers, the whole frame is
    // composited on the CPU and uploaded.
    //
    // The result is not merely close to the export: it is the same code. The
    // cost is a slow frame wherever an adjustment layer is, and that is a
    // trade worth stating rather than hiding.
    if (hasAdjustment(sequence, at) && nestedSource_ != nullptr) {
        if (nested_ == nullptr) {
            nested_ = std::make_unique<render::RenderGraph>(*nestedSource_);
            nested_->setProject(project_);
            nested_->setTextRasterizer(text_);
        }
        auto frame = nested_->composite(sequence, at);
        if (!frame) {
            return frame.error();
        }
        lastClipCount_ = nested_->lastClipCount();
        return compositor_->draw(*frame, model::Transform{}, model::BlendMode::Normal);
    }

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

                if (outgoing->enabled) {
                    if (auto frame = provider_->sourceFrameFor(outgoing->activeSource(),
                                                               outgoing->activeSourceTimeAt(at))) {
                        if (drawClip(*outgoing, **frame, outgoing->transformAt(at), at)) {
                            ++lastClipCount_;
                        }
                    }
                }
                if (incoming->enabled) {
                    if (auto frame = provider_->sourceFrameFor(incoming->activeSource(),
                                                               incoming->activeSourceTimeAt(at))) {
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
            }
            auto frame = nested_->composite(*inner, clip->sourceTimeAt(at));
            if (!frame) {
                continue;
            }
            if (drawClipImage(*clip, *frame, clip->transformAt(at), at)) {
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
                if (!render::drawText(clip->graphic, text_, generated_)) {
                    continue;
                }
            } else {
                render::drawShape(clip->graphic, generated_);
            }
            if (drawClipImage(*clip, generated_, clip->transformAt(at), at)) {
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
        if (!drawClip(*clip, **frame, clip->transformAt(at), at)) {
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
