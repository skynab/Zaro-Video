#include "zaro/core/render/RenderGraph.h"

#include "zaro/core/render/EffectStack.h"
#include "zaro/core/render/Grade.h"
#include "zaro/core/render/Keyer.h"
#include "zaro/core/render/PathRaster.h"
#include "zaro/core/render/RenderCache.h"
#include "zaro/core/render/ShapeRaster.h"
#include "zaro/core/render/TextRasterizer.h"
#include "zaro/core/render/TransitionShape.h"

namespace zaro::render {

/// Draw one clip's frame, with everything that applies to it.
///
/// A single place on purpose. The three call sites below -- an ordinary clip
/// and the two halves of a transition -- each used to compute the grade for
/// themselves, and they drifted: the outgoing half of a transition went two
/// phases without its colour correction, because a patch that was meant to add
/// it silently did not match that copy of the code.
const std::vector<float>* RenderGraph::coverageFor(const model::Mask& mask, std::int32_t width,
                                                   std::int32_t height) {
    if (mask.shape != model::MaskShape::Path || !mask.path.isSet()) {
        return nullptr;
    }
    // Kept until the mask or the frame size changes. Grading through a path
    // mask means re-rasterising it on every frame otherwise, and a scanline
    // fill of a 4K frame is not something to do for a mask nobody has touched.
    if (pathCoverageWidth_ != width || pathCoverageHeight_ != height ||
        !(pathCoverageFor_ == mask)) {
        rasteriseMaskPath(mask.path, mask.feather, mask.inverted, width, height, pathCoverage_);
        pathCoverageFor_ = mask;
        pathCoverageWidth_ = width;
        pathCoverageHeight_ = height;
    }
    return &pathCoverage_;
}

void RenderGraph::drawClip(const model::Clip& clip, const RgbaImage& image, RgbaImage& out,
                           const model::Transform& transform, const time::RationalTime& at,
                           const model::Mask* wipe) {
    const GradeConstants grade = gradeConstantsFor(clip.colorAt(at), clip.wheels);
    const CurveTable& table = curves_.tableFor(clip.id.value(), clip.curves, transfer_);
    const SecondaryConstants secondary = secondaryConstantsFor(clip.secondary, transfer_);
    const LutTable* lut = clip.lut.isSet() ? luts_.tableFor(clip.lut.path, transfer_) : nullptr;
    // Effects first, and on the image rather than on a sampled pixel: they are
    // the only stage that reads a pixel's neighbours, so there is nothing
    // useful to hand them one pixel at a time.
    const RgbaImage* source = &image;
    if (model::anyActive(clip.effects)) {
        effected_ = image.clone();
        applyEffects(clip.effects, effected_, effectScratch_, effectScratchB_,
                     clip.animationSecondsAt(at));
        source = &effected_;
    }

    const KeyerConstants keyer = keyerConstantsFor(clip.keyer, transfer_);
    // Sampled here rather than read off the clip: a tracked mask moves frame
    // by frame, and everything below this line has to see where it is now.
    const model::Mask mask = clip.maskAt(at);
    // A mask alone is reason enough to take the slow path: it changes which
    // pixels are drawn even when nothing about their colour does. So is a key,
    // which changes whether they are drawn at all.
    const bool active = !grade.isIdentity() || !table.isIdentity() || secondary.isActive() ||
                        lut != nullptr || mask.isSet() || keyer.isActive() ||
                        clip.vignette.isSet() || (wipe != nullptr && wipe->isSet());
    ClipShading shading;
    shading.grade = active ? &grade : nullptr;
    shading.curves = active ? &table : nullptr;
    shading.secondary = active ? &secondary : nullptr;
    shading.lut = lut;
    shading.lutAmount = static_cast<float>(clip.lut.amount);
    shading.mask = mask.isSet() ? &mask : nullptr;
    shading.keyer = keyer.isActive() ? &keyer : nullptr;
    shading.vignette = clip.vignette.isSet() ? &clip.vignette : nullptr;
    shading.wipe = wipe;
    if (const std::vector<float>* coverage = coverageFor(mask, out.width(), out.height())) {
        // A path answers coverage from a buffer, so the analytic mask slot is
        // left alone rather than being handed a shape it cannot describe.
        shading.mask = nullptr;
        shading.pathCoverage = coverage->data();
        shading.pathWidth = out.width();
    }
    drawTransformed(*source, out, transform, clip.blend, shading);
}

void RenderGraph::applyAdjustment(const model::Clip& clip, RgbaImage& out,
                                  const time::RationalTime& at) {
    const GradeConstants grade = gradeConstantsFor(clip.colorAt(at), clip.wheels);
    const CurveTable& table = curves_.tableFor(clip.id.value(), clip.curves, transfer_);
    const SecondaryConstants secondary = secondaryConstantsFor(clip.secondary, transfer_);
    const LutTable* lut = clip.lut.isSet() ? luts_.tableFor(clip.lut.path, transfer_) : nullptr;
    if (grade.isIdentity() && table.isIdentity() && !secondary.isActive() && lut == nullptr) {
        return;
    }

    const model::Transform transform = clip.transformAt(at);
    const auto opacity = static_cast<float>(std::clamp(transform.opacity, 0.0, 1.0));
    if (opacity <= 0.0F) {
        return;
    }

    const model::Mask mask = clip.maskAt(at);
    // In place, over what is already there. An adjustment layer is not drawn
    // and then composited -- it changes what has been composited, which is why
    // it cannot be expressed as a clip that draws something.
    for (std::int32_t y = 0; y < out.height(); ++y) {
        Rgba* row = out.row(y);
        for (std::int32_t x = 0; x < out.width(); ++x) {
            Rgba& pixel = row[x];
            if (pixel.a <= 0.0001F) {
                continue;  // nothing underneath to adjust
            }
            float coverage = opacity;
            if (mask.isSet()) {
                coverage *= maskCoverage(mask, out.width(), out.height(), x, y);
            }
            if (coverage <= 0.0F) {
                continue;
            }

            const float inverse = 1.0F / pixel.a;
            float r = pixel.r * inverse;
            float g = pixel.g * inverse;
            float b = pixel.b * inverse;
            const float wasR = r;
            const float wasG = g;
            const float wasB = b;
            gradePixel(grade, r, g, b, &table, &secondary, lut,
                       static_cast<float>(clip.lut.amount));
            // Blended by opacity and mask rather than switched, so a partly
            // opaque adjustment is a partial correction -- which is how the
            // control reads.
            r = wasR + ((r - wasR) * coverage);
            g = wasG + ((g - wasG) * coverage);
            b = wasB + ((b - wasB) * coverage);
            pixel.r = r * pixel.a;
            pixel.g = g * pixel.a;
            pixel.b = b * pixel.a;
        }
    }
}

const RgbaImage* RenderGraph::clipImage(const model::Clip& clip, const time::RationalTime& at,
                                        RgbaImage& scratch, std::int32_t width,
                                        std::int32_t height) {
    if (clip.graphic.isSet()) {
        // Generated rather than read, at the sequence's size, so the transform
        // that follows means the same thing it does for a clip whose media
        // happens to be that size.
        if (scratch.width() != width || scratch.height() != height) {
            scratch = RgbaImage{width, height};
        }
        if (clip.graphic.kind == model::GraphicKind::Text) {
            if (!drawText(clip.graphic, text_, scratch)) {
                ++skippedText_;
                return nullptr;
            }
        } else {
            drawShape(clip.graphic, scratch);
        }
        return &scratch;
    }
    auto image = source_->imageFor(clip.activeSource(), clip.activeSourceTimeAt(at));
    if (!image) {
        // One unreadable clip must not take the whole frame with it. A gap
        // where a clip should be is a visible, diagnosable problem; a failed
        // render is a stalled edit.
        return nullptr;
    }
    return *image;
}

bool RenderGraph::compositeNested(const model::Sequence& sequence, const model::Clip& clip,
                                  RgbaImage& out, const time::RationalTime& at) {
    if (project_ == nullptr) {
        return false;
    }
    const model::Sequence* inner = project_->findSequence(clip.nested);
    if (inner == nullptr) {
        return false;
    }
    // A backstop, not the defence. Cycles are refused when the edit is made
    // (Project::nestingWouldCycle); this is here so that a project which
    // arrived from somewhere else -- an OTIO file, a hand-edited save -- cannot
    // take the renderer down with it.
    constexpr std::int32_t kMaxDepth = 8;
    if (depth_ >= kMaxDepth) {
        return false;
    }

    if (static_cast<std::size_t>(depth_) >= nestedBuffers_.size()) {
        nestedBuffers_.resize(static_cast<std::size_t>(depth_) + 1);
    }
    RgbaImage& buffer = nestedBuffers_[static_cast<std::size_t>(depth_)];

    // compositeInto resets the counters, and the recursive call would therefore
    // wipe the tally the level above is still building. Saved across it: the
    // outer count is what the outer sequence drew, and a nested sequence
    // contributes one clip to it however many it contains.
    const std::int32_t outerClips = lastClipCount_;
    const std::int32_t outerSkipped = skippedText_;

    ++depth_;
    const Status composed = compositeInto(*inner, clip.sourceTimeAt(at), buffer);
    --depth_;

    const std::int32_t nestedSkipped = skippedText_;
    lastClipCount_ = outerClips;
    skippedText_ = outerSkipped + nestedSkipped;
    if (!composed) {
        return false;
    }

    drawClip(clip, buffer, out, pinnedTransformAt(sequence, clip, at), at);
    return true;
}

Status RenderGraph::compositeInto(const model::Sequence& sequence, const time::RationalTime& at,
                                  RgbaImage& out) {
    if (sequence.width() <= 0 || sequence.height() <= 0) {
        return Error{ErrorCode::InvalidData, "the sequence has no frame size"};
    }

    // Only at the top. A nested composite is part of the outer frame's recipe,
    // so caching it separately would store the same work twice.
    const bool cacheable = cache_ != nullptr && depth_ == 0;
    std::uint64_t recipe = 0;
    if (cacheable) {
        recipe = frameRecipe(project_, sequence, at);
        if (const RenderCache::Entry hit = cache_->find(sequence.id(), at, recipe);
            hit.image != nullptr) {
            out = hit.image->clone();
            // The counters describe the frame, not the work: a cached frame
            // still had three clips on it, and a caller showing "3 layers"
            // must not have it flicker to zero when the cache answers.
            lastClipCount_ = hit.clipCount;
            skippedText_ = hit.skippedText;
            return {};
        }
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
    // Curves, secondaries, LUTs and keys are all defined against the picture as
    // it is *shown*, so they need the curve this sequence is delivered through.
    // Read here rather than set by the caller: a nested sequence composites
    // through this same function, and one that had to be told separately would
    // eventually be told something different from its parent.
    transfer_ = sequence.output().transfer;

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
                    if (const RgbaImage* image =
                            clipImage(*outgoing, at, generated_, out.width(), out.height())) {
                        drawClip(*outgoing, *image, out, pinnedTransformAt(sequence, *outgoing, at),
                                 at);
                        ++lastClipCount_;
                    }
                }
                if (incoming->enabled) {
                    if (const RgbaImage* image =
                            clipImage(*incoming, at, generatedB_, out.width(), out.height())) {
                        // Drawn over the outgoing clip at the dissolve's
                        // progress: with premultiplied `over` and an opaque
                        // source that gives out*(1-p) + in*p.
                        // One function decides what a transition looks like
                        // part way through, and both render paths call it.
                        const TransitionShape shape =
                            transitionShapeFor(*transition, progress, out.width(), out.height());
                        model::Transform moving = pinnedTransformAt(sequence, *incoming, at);
                        moving.opacity *= shape.opacity;
                        moving.positionX += shape.offsetX;
                        moving.positionY += shape.offsetY;
                        drawClip(*incoming, *image, out, moving, at,
                                 shape.wipe.isSet() ? &shape.wipe : nullptr);
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

        if (clip->adjustment) {
            applyAdjustment(*clip, out, at);
            ++lastClipCount_;
            continue;
        }

        if (clip->nested.isValid()) {
            if (!compositeNested(sequence, *clip, out, at)) {
                continue;
            }
            ++lastClipCount_;
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
            drawClip(*clip, generated_, out, pinnedTransformAt(sequence, *clip, at), at);
            ++lastClipCount_;
            continue;
        }

        auto image = source_->imageFor(clip->activeSource(), clip->activeSourceTimeAt(at));
        if (!image) {
            // One unreadable clip must not take the whole frame with it. A gap
            // where a clip should be is a visible, diagnosable problem; a failed
            // render is a stalled edit.
            continue;
        }
        drawClip(*clip, **image, out, pinnedTransformAt(sequence, *clip, at), at);
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

    if (cacheable) {
        cache_->insert(sequence.id(), at, recipe, out.clone(), lastClipCount_, skippedText_);
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
