#pragma once

#include <string>
#include <vector>

#include "zaro/core/Error.h"
#include "zaro/core/model/Ids.h"
#include "zaro/core/render/FrameSource.h"
#include "zaro/core/time/RationalTime.h"

namespace zaro::render {

/// Where the beats are, in seconds from the start of the media.
///
/// **Onset strength, then peaks.** The rise in energy from one short window to
/// the next is what a beat is from outside the music: a drum, a chord change, a
/// note starting. Rises are what count -- a fall in energy is the end of
/// something, and no listener claps on those.
///
/// **Not a tempo estimate.** A grid fitted to a tempo is a better answer for
/// music that keeps one and a worse answer for music that does not, and the
/// only thing this is used for is picking somewhere to cut, which wants the
/// real onsets rather than the average of them.
[[nodiscard]] Result<std::vector<double>> detectBeats(AudioSource& source, model::MediaRefId media,
                                                      double seconds,
                                                      const time::Rational& sampleRate);

/// How to make a piece of music a given length by taking a piece out of it.
struct RemixPlan {
    /// Play from the start up to here, then jump to `resumeFrom`.
    double cutAt{0.0};
    double resumeFrom{0.0};
    /// How long each half fades at the join, in seconds.
    ///
    /// **A dip, not a crossfade.** A crossfade needs the two halves to overlap,
    /// and two clips on one track cannot: the timeline holds that clips are in
    /// order and do not overlap, which is what makes everything else about it
    /// simple. So the outgoing half fades down into the cut and the incoming
    /// half fades up out of it. That is what hides the click of a chopped note
    /// tail; a real crossfade wants audio transitions, which this model does
    /// not have yet.
    double joinFade{0.03};

    /// What the result actually comes to, which is within a beat of what was
    /// asked for rather than exactly it -- the alternative is cutting off the
    /// beat, which is the one thing this exists to avoid.
    double seconds{0.0};
    /// Whole beats removed, for saying what happened.
    int beatsRemoved{0};
};

/// Work out where to cut a track so it comes to `targetSeconds`.
///
/// **Both edges land on onsets, and the gap between them is a whole number of
/// them.** Cutting on a beat is what stops the join being audible as a stumble;
/// removing a whole number of beats is what stops the music arriving on the
/// wrong foot afterwards.
///
/// **It refuses to make music longer.** Looping to extend is a different
/// operation with a different failure -- the join is heard twice -- and doing
/// it silently under the name of "fit to length" would be a surprise.
///
/// **It refuses when there is nothing to cut on.** Fading a track out early is
/// something an editor can do in two gestures and knows they have done; a
/// remix that quietly stopped being a remix is worse than being told no.
[[nodiscard]] Result<RemixPlan> planRemix(const std::vector<double>& beats, double sourceSeconds,
                                          double targetSeconds);

}  // namespace zaro::render
