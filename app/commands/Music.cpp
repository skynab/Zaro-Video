// Fitting music to a length.

#include "Music.h"

#include "zaro/core/edit/Operations.h"
#include "zaro/core/render/Remix.h"

namespace zaro::app::commands {

/// Fit the selected music clip to a length by taking a piece out of it.
///
/// The length wanted is the picture's: fitting music to a cut is the
/// errand, and asking for a number when the answer is on screen would be a
/// question with one sensible reply.
Result<render::RemixPlan> remixSelectedTo(const Context& context, double targetSeconds) {
    const model::Sequence* sequence = context.sequence();
    const model::Clip* clip = context.selectedClip();
    if (clip == nullptr || context.media == nullptr) {
        return Error{ErrorCode::InvalidData, "select the music to fit"};
    }
    if (!clip->activeSource().isValid()) {
        return Error{ErrorCode::InvalidData, "that clip has no media to look at"};
    }
    const model::MediaRef* media = context.project().findMedia(clip->activeSource());
    if (media == nullptr || media->info.primaryAudio() == nullptr) {
        return Error{ErrorCode::InvalidData, "there is no sound in that clip"};
    }

    const double have = clip->sourceRange.duration().toSecondsDouble();
    auto beats = render::detectBeats(*context.media, clip->activeSource(), have,
                                     sequence->audioSampleRate());
    if (!beats) {
        return beats.error();
    }
    // The beats are measured from the clip's own start, so a clip already
    // trimmed into the middle of a track still cuts on its own beats.
    auto plan = render::planRemix(*beats, have, targetSeconds);
    if (!plan) {
        return plan;
    }

    auto built = edit::makeRemix(context.project(), context.target(), context.clip, plan->cutAt,
                                 plan->resumeFrom, plan->joinFade);
    if (!built) {
        return built.error();
    }
    context.commands().execute(context.project(), std::move(*built));
    context.commands().breakMerge();
    return plan;
}

}  // namespace zaro::app::commands
