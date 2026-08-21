#pragma once

#include <vector>

#include "zaro/core/Error.h"
#include "zaro/core/model/Animation.h"
#include "zaro/core/model/Project.h"
#include "zaro/core/render/FrameSource.h"
#include "zaro/core/time/TimeRange.h"

namespace zaro::render {

struct DuckingOptions {
    /// How far the music comes down while somebody is speaking.
    double duckDb{-12.0};

    /// How long it takes to get there, and to come back.
    ///
    /// Asymmetric on purpose. Coming down late is audible as the first word
    /// being buried; going back up early is audible as a pump under the pause
    /// between sentences. So the fade down is quick and the fade up is slow,
    /// which is what a hand-drawn ducking curve looks like.
    time::RationalTime fadeDown{7200, time::Rational{48000, 1}};  // 0.15s
    time::RationalTime fadeUp{28800, time::Rational{48000, 1}};   // 0.6s

    /// How long the dialogue has to stay quiet before the music comes back.
    ///
    /// Without it the music lifts between every sentence, which is more
    /// distracting than never ducking at all.
    time::RationalTime hold{28800, time::Rational{48000, 1}};  // 0.6s

    /// How loud the dialogue has to be to count as speech, as a linear
    /// amplitude. Room tone sits well below this; a voice sits well above.
    double threshold{0.02};

    /// The rate the dialogue is examined at.
    time::Rational sampleRate{48000, 1};
};

/// Work out a gain curve for one clip that keeps out of the way of others.
///
/// **It follows what is heard, not what is on the timeline.** A dialogue clip
/// with ten seconds of room tone at its head would otherwise duck the music for
/// ten seconds before anybody said anything. So the dialogue is read and its
/// loudness envelope is what decides, which also means a pause long enough to
/// matter lifts the music without anyone marking it.
///
/// **The answer is keyframes, not a live sidechain.** A compressor listening to
/// another track would be fewer moving parts and completely opaque: nothing on
/// screen would say why the music dipped, and nothing could be nudged when it
/// dipped in the wrong place. Keyframes are the same automation somebody would
/// have drawn, on the parameter they would have drawn it on, and they can be
/// dragged afterwards.
///
/// Keyframes come back in the ducked clip's **source** time, like every other
/// curve on a clip (ADR-008), so the ducking stays glued to the music through a
/// trim.
[[nodiscard]] Result<model::Curve> duckingCurve(const model::Sequence& sequence,
                                                const model::Clip& ducked, AudioSource& audio,
                                                const DuckingOptions& options = {});

}  // namespace zaro::render
