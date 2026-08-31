#pragma once

#include <cstdint>
#include <span>

#include "zaro/core/model/Clip.h"
#include "zaro/core/model/Track.h"

namespace zaro::model {

/// What a clip *is*, as one answer rather than five separate questions.
///
/// The facts are already on the clip -- a graphic kind, an adjustment flag, a
/// nested sequence id, a list of angles -- and everything that needs to treat
/// one kind differently was re-deriving its own subset of them: the inspector
/// asked the track, the render graph asks the flag, the timeline asks the
/// graphic. Three places asking three overlapping questions is three chances
/// to disagree about what a clip with two of those things set is.
///
/// So the classification is made once, here, and the precedence below is the
/// answer to "which wins" rather than something each caller invents.
enum class ClipKind : std::uint8_t {
    /// A clip that reads a picture from a file. The ordinary case.
    VideoMedia,
    /// A clip that reads sound from a file. Distinguished from `VideoMedia` by
    /// the track it sits on, which is the only thing that separates them: a
    /// file with both streams is cut as two linked clips, one on each.
    AudioMedia,
    /// A generated rectangle or ellipse.
    Shape,
    /// A generated text layer.
    Text,
    /// A clip that grades what is beneath it instead of drawing anything.
    Adjustment,
    /// Several angles, one of them live.
    Multicam,
    /// Another sequence, used as a clip.
    Nested,
};

[[nodiscard]] const char* toString(ClipKind kind) noexcept;
[[nodiscard]] std::span<const ClipKind> allClipKinds() noexcept;

/// What this clip is, on a track of this kind.
///
/// The track is needed because picture and sound are otherwise identical: a
/// clip carries a gain and a transform whichever it is, and which of them means
/// anything is decided by where it sits.
///
/// Precedence, most specific first: sound before everything (nothing else on
/// this list is audible); then the generated kinds, which have no media to be
/// anything else about; then nested and multicam, which do. A clip that somehow
/// had two of these set gets the earlier one, and the render graph agrees --
/// it checks `adjustment` before it looks for a picture to draw.
[[nodiscard]] ClipKind clipKindOf(const Clip& clip, TrackKind track) noexcept;

/// Whether this kind draws a picture of its own that can be placed in the frame.
///
/// False for sound, which has no picture, and for an adjustment layer, which
/// has no picture *of its own* -- moving one would mean moving the correction
/// off the thing it was made for. This is the question the motion controls are
/// asking.
[[nodiscard]] bool hasPicture(ClipKind kind) noexcept;

/// Whether this kind reads from a file, rather than generating what it shows.
///
/// The generated kinds have no source to relink, no media to report, no frames
/// to analyse, and nothing for a keyer to pull -- a shape is already exactly as
/// transparent as it was authored to be.
[[nodiscard]] bool readsMedia(ClipKind kind) noexcept;

}  // namespace zaro::model
