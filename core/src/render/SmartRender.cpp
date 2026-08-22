#include "zaro/core/render/SmartRender.h"

namespace zaro::render {
namespace {

/// Whether anything at all has been done to this clip's picture.
///
/// A list of defaults rather than a judgement. Each line is a thing that, if
/// set, means the exported picture is not the source's picture.
[[nodiscard]] bool isUntouched(const model::Clip& clip, std::string& why) {
    const auto no = [&why](const char* reason) {
        why = reason;
        return false;
    };
    if (!clip.enabled) {
        return no("the clip is switched off");
    }
    if (clip.graphic.isSet() || clip.nested.isValid() || clip.adjustment) {
        return no("that is not a piece of a file: it is generated, nested or an adjustment");
    }
    if (clip.isMulticam()) {
        return no("a multicam clip reads whichever angle is live");
    }
    if (clip.reversed) {
        return no("the clip is reversed");
    }
    if (clip.timelineRange.duration() != clip.sourceRange.duration()) {
        return no("the clip has been retimed");
    }
    if (!clip.transform.isIdentity()) {
        return no("the clip has been moved, scaled or faded");
    }
    if (clip.blend != model::BlendMode::Normal) {
        return no("the clip has a blend mode");
    }
    if (!clip.animation.empty()) {
        return no("something on the clip is keyframed");
    }
    if (clip.color != model::ColorCorrection{} || clip.wheels != model::ColorWheels{}) {
        return no("the clip is graded");
    }
    if (!clip.curves.isIdentity() || clip.secondary.isActive() || clip.lut.isSet()) {
        return no("the clip has curves, a secondary or a LUT on it");
    }
    if (clip.mask.isSet() || clip.vignette.isSet() || clip.keyer.isSet()) {
        return no("the clip is masked, vignetted or keyed");
    }
    // An effect list whose entries are all at their defaults changes nothing,
    // so it does not stop a copy; an active one does.
    if (model::anyActive(clip.effects)) {
        return no("the clip has effects on it");
    }
    if (clip.pinnedTo.isValid()) {
        return no("the clip is pinned to another");
    }
    if (clip.responsive.isSet()) {
        return no("the clip has responsive timing");
    }
    return true;
}

}  // namespace

SmartRenderPlan smartRenderPlan(const model::Project& project, const model::Sequence& sequence,
                                std::int64_t startFrame, std::int64_t frameCount,
                                const SmartRenderTarget& target) {
    SmartRenderPlan plan;
    const auto rate = sequence.frameRate();
    if (frameCount <= 0) {
        plan.reason = "there is nothing to export";
        return plan;
    }
    if (target.videoCodec.empty()) {
        plan.reason = "the export has not said which codec it wants";
        return plan;
    }
    if (sequence.width() != target.width || sequence.height() != target.height ||
        sequence.frameRate() != target.frameRate) {
        plan.reason = "the export is a different size or rate from the sequence";
        return plan;
    }
    if (sequence.captions().isBurnedIn()) {
        plan.reason = "the captions are burned in";
        return plan;
    }

    const time::RationalTime from{startFrame, rate};
    const time::RationalTime to{startFrame + frameCount, rate};

    const model::Clip* only = nullptr;
    for (const model::Track& track : sequence.videoTracks()) {
        if (!sequence.isAudible(track)) {
            continue;
        }
        // Anything on a second visible track is something to composite, even
        // if it is only a title at the end.
        for (const model::Clip& clip : track.clips()) {
            if (clip.endExclusive() <= from || clip.start() >= to) {
                continue;
            }
            if (only != nullptr) {
                plan.reason = "more than one clip is on screen over that range";
                return plan;
            }
            only = &clip;
        }
        if (!track.transitions().empty()) {
            for (const model::Transition& transition : track.transitions()) {
                if (transition.range.endExclusive() > from && transition.range.start() < to) {
                    plan.reason = "there is a transition in that range";
                    return plan;
                }
            }
        }
    }
    if (only == nullptr) {
        plan.reason = "there is nothing on screen over that range";
        return plan;
    }
    if (only->start() > from || only->endExclusive() < to) {
        plan.reason = "the clip does not cover the whole range";
        return plan;
    }
    if (!isUntouched(*only, plan.reason)) {
        return plan;
    }

    const model::MediaRef* media = project.findMedia(only->activeSource());
    if (media == nullptr) {
        plan.reason = "that clip's media is missing";
        return plan;
    }
    if (project.usingProxies() && !media->proxyPath.empty()) {
        // Copying the proxy into a deliverable is the one mistake that would
        // ship at the wrong quality without anybody noticing.
        plan.reason = "the project is on proxies";
        return plan;
    }
    const media::VideoStreamInfo* video = media->info.primaryVideo();
    if (video == nullptr) {
        plan.reason = "that file has no picture in it";
        return plan;
    }
    if (video->width != target.width || video->height != target.height) {
        plan.reason = "the file is a different size from the export";
        return plan;
    }
    if (video->frameRate != target.frameRate) {
        plan.reason = "the file runs at a different rate from the export";
        return plan;
    }
    if (video->isVariableFrameRate) {
        plan.reason = "the file has a variable frame rate";
        return plan;
    }
    if (video->codecName.empty() || video->codecName != target.videoCodec) {
        plan.reason = "the export is in a different codec from the file";
        return plan;
    }

    if (target.includeAudio) {
        const media::AudioStreamInfo* audio = media->info.primaryAudio();
        if (audio == nullptr || audio->sampleRate != target.audioSampleRate ||
            audio->channelCount != target.audioChannels) {
            plan.reason = "the audio would have to be re-encoded";
            return plan;
        }
        if (only->gainDb != 0.0 || only->pan != 0.0) {
            plan.reason = "the clip's level or pan has been changed";
            return plan;
        }
        plan.copyAudio = true;
    }

    plan.possible = true;
    plan.reason = "every frame is a copy of the file";
    plan.media = only->activeSource();
    plan.sourceStart = only->sourceTimeAt(from);
    plan.frames = frameCount;
    return plan;
}

}  // namespace zaro::render
