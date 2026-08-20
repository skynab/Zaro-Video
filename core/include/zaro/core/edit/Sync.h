#pragma once

#include <optional>
#include <string>
#include <vector>

#include "zaro/core/Error.h"
#include "zaro/core/model/Clip.h"
#include "zaro/core/model/Project.h"
#include "zaro/core/render/FrameSource.h"
#include "zaro/core/time/RationalTime.h"

namespace zaro::edit {

/// What was worked out for one angle of a multicam clip.
///
/// Per angle rather than all-or-nothing, because the usual failure is one
/// camera: the one that was not jam-synced, or the one whose audio is a hiss
/// from across the room. Refusing to sync the other three because of it would
/// be worse than saying which one could not be done.
struct AngleSync {
    std::int32_t angle{0};

    /// What to store in `Clip::Angle::offset`. Empty when this angle could not
    /// be synced, in which case `reason` says why and the existing offset is
    /// left alone.
    std::optional<time::RationalTime> offset;

    /// 1.0 for timecode, which is either there and exact or absent. For audio,
    /// the correlation at the answer.
    double confidence{0.0};

    std::string reason;
};

/// Sync the angles from their source timecode.
///
/// **Exact when it works, and it either works or it does not.** Timecode from a
/// jam-synced shoot is the same clock written into every file; the offsets fall
/// out as subtraction, with nothing to be confident about. A camera that wrote
/// no timecode -- or wrote its own free-running one -- is not a near miss to be
/// reported with low confidence, it is a camera this method cannot do, and
/// saying so is more use than a plausible wrong answer.
///
/// The first angle is the reference, the same one an out-of-range angle falls
/// back to, so that "which camera is time zero" has one answer everywhere.
[[nodiscard]] Result<std::vector<AngleSync>> syncByTimecode(const model::Project& project,
                                                            const model::Clip& clip);

struct AudioSyncOptions {
    /// How much of each angle to listen to, from the start of its material.
    ///
    /// A minute is long enough to contain the clap, the slate or the first
    /// words that every take has, and short enough that reading it is not a
    /// noticeable wait.
    time::RationalTime window{60 * 48000, time::Rational{48000, 1}};

    /// How far apart the cameras may have started rolling. The search costs
    /// time in proportion, and a range wider than could plausibly have happened
    /// only adds places for a wrong answer to hide.
    time::RationalTime maxOffset{30 * 48000, time::Rational{48000, 1}};

    /// The rate the comparison is done at. Lower is faster and no less
    /// accurate for this purpose: what is being matched is the shape of the
    /// loudness over time, and that survives resampling.
    time::Rational sampleRate{48000, 1};

    /// Below this, the offset is returned with its confidence but marked as
    /// not worth applying.
    double minimumConfidence{0.5};
};

/// Sync the angles by ear.
///
/// For a shoot where the cameras were not jam-synced, which is most of them.
/// The signal work is in `media::align`; this reads the audio, mixes each angle
/// to mono and turns samples into times.
///
/// **Mono, not one channel.** A camera with a dead left input and a camera with
/// a dead right one would otherwise fail to correlate with each other despite
/// having recorded the same room.
[[nodiscard]] Result<std::vector<AngleSync>> syncByAudio(const model::Project& project,
                                                         const model::Clip& clip,
                                                         render::AudioSource& audio,
                                                         const AudioSyncOptions& options = {});

}  // namespace zaro::edit
