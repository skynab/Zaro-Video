#include "zaro/core/render/RenderGraph.h"

#include "zaro/core/render/Grade.h"
#include "zaro/core/render/ShapeRaster.h"
#include "zaro/core/render/TextRasterizer.h"

namespace zaro::render {

/// Draw one clip's frame, with everything that applies to it.
///
/// A single place on purpose. The three call sites below -- an ordinary clip
/// and the two halves of a transition -- each used to compute the grade for
/// themselves, and they drifted: the outgoing half of a transition went two
/// phases without its colour correction, because a patch that was meant to add
/// it silently did not match that copy of the code.
void RenderGraph::drawClip(const model::Clip& clip, const RgbaImage& image, RgbaImage& out,
                           const model::Transform& transform, const time::RationalTime& at) {
    const GradeConstants grade = gradeConstantsFor(clip.colorAt(at));
    const CurveTable& table = curves_.tableFor(clip.id.value(), clip.curves, transfer_);
    const SecondaryConstants secondary = secondaryConstantsFor(clip.secondary, transfer_);
    const LutTable* lut = clip.lut.isSet() ? luts_.tableFor(clip.lut.path, transfer_) : nullptr;
    const bool active =
        !grade.isIdentity() || !table.isIdentity() || secondary.isActive() || lut != nullptr;
    drawTransformed(image, out, transform, clip.blend, active ? &grade : nullptr,
                    active ? &table : nullptr, active ? &secondary : nullptr, lut,
                    static_cast<float>(clip.lut.amount));
}

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
    skippedText_ = 0;

    // Bottom-up. Index 0 is V1, the lowest track, and each later track
    // composites over what is already there.
    for (const model::Track& track : sequence.videoTracks()) {
        if (!sequence.isAudible(track)) {
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
                        drawClip(*outgoing, **image, out, outgoing->transformAt(at), at);
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
                        drawClip(*incoming, **image, out, fading, at);
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

        if (clip->graphic.isSet()) {
            // Generated rather than read. Drawn at the sequence's size, so the
            // transform that follows means the same thing it does for a clip
            // whose media happens to be that size.
            if (generated_.width() != out.width() || generated_.height() != out.height()) {
                generated_ = RgbaImage{out.width(), out.height()};
            }
            if (clip->graphic.kind == model::GraphicKind::Text) {
                if (!drawText(clip->graphic, text_, generated_)) {
                    // No rasteriser, or it failed. Counted rather than
                    // ignored, so a caller can say "this render had no font
                    // engine" instead of leaving someone to notice the missing
                    // title in the delivered file.
                    ++skippedText_;
                    continue;
                }
            } else {
                drawShape(clip->graphic, generated_);
            }
            drawClip(*clip, generated_, out, clip->transformAt(at), at);
            ++lastClipCount_;
            continue;
        }

        auto image = source_->imageFor(clip->source, clip->sourceTimeAt(at));
        if (!image) {
            // One unreadable clip must not take the whole frame with it. A gap
            // where a clip should be is a visible, diagnosable problem; a failed
            // render is a stalled edit.
            continue;
        }
        drawClip(*clip, **image, out, clip->transformAt(at), at);
        ++lastClipCount_;
    }

    // Captions last, over everything: they are a deliverable laid on top of the
    // picture rather than a layer in it, and a caption a later track could
    // cover is a caption nobody can read.
    if (sequence.captions().isBurnedIn()) {
        for (const model::Caption* caption : sequence.captions().at(at)) {
            if (generated_.width() != out.width() || generated_.height() != out.height()) {
                generated_ = RgbaImage{out.width(), out.height()};
            }
            const model::Graphic graphic = captionGraphic(sequence.captions().style(),
                                                          caption->text, out.width(), out.height());
            if (drawText(graphic, text_, generated_)) {
                drawOver(generated_, out);
            } else {
                ++skippedText_;
            }
        }
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
