#pragma once

#include <string>

#include "zaro/core/Error.h"
#include "zaro/core/edit/Command.h"
#include "zaro/core/model/ClipEffects.h"
#include "zaro/core/model/Project.h"

namespace zaro::edit {

/// Which sequence and track an edit lands on.
struct EditTarget {
    model::SequenceId sequence;
    model::TrackId track;
};

/// Which end of a clip a trim moves.
enum class Edge { In, Out };

/// Every operation is built, validated and only then executed.
///
/// Validation happens here, before the command exists, so an edit that cannot
/// work fails without having touched the model. The alternative -- validating
/// inside apply() -- leaves the question of what to do about the half of the
/// edit that already happened.
///
/// Deltas are always in the sequence's frame rate, and positive always means
/// later in time. An in-point moved by +5 shortens the clip; an out-point moved
/// by +5 lengthens it.

// --- Placement --------------------------------------------------------------

/// Drop a clip at its timeline range, cutting away whatever is underneath.
/// Clips partially covered are trimmed; a clip spanning the whole insertion is
/// split in two around it.
[[nodiscard]] Result<CommandPtr> makeOverwrite(model::Project& project, const EditTarget& target,
                                               model::Clip clip);

/// Push everything at the insertion point to the right and drop the clip into
/// the space. A clip straddling the point is split rather than moved whole,
/// which is what keeps the edit from silently shifting picture already cut.
///
/// `rippleAllTracks` shifts every track in the sequence, not just the target --
/// the behaviour that keeps sync when inserting into a multi-track cut.
[[nodiscard]] Result<CommandPtr> makeInsert(model::Project& project, const EditTarget& target,
                                            model::Clip clip, bool rippleAllTracks = false);

/// Move a clip, optionally to another track. Overwrite semantics at the
/// destination.
[[nodiscard]] Result<CommandPtr> makeMove(model::Project& project, const EditTarget& target,
                                          model::ClipId clip, model::TrackId toTrack,
                                          const time::RationalTime& newStart);

// --- Cutting ----------------------------------------------------------------

/// Split the clip under `at` into two abutting clips. `at` must fall strictly
/// inside a clip: splitting on a boundary would produce a zero-length clip.
[[nodiscard]] Result<CommandPtr> makeRazor(model::Project& project, const EditTarget& target,
                                           const time::RationalTime& at);

/// Remove a clip and leave a gap where it was.
[[nodiscard]] Result<CommandPtr> makeLift(model::Project& project, const EditTarget& target,
                                          model::ClipId clip);

/// Remove a clip and close the gap.
[[nodiscard]] Result<CommandPtr> makeExtract(model::Project& project, const EditTarget& target,
                                             model::ClipId clip);

/// Remove everything in a range and close the gap, trimming clips that only
/// partly overlap.
[[nodiscard]] Result<CommandPtr> makeRippleDelete(model::Project& project, const EditTarget& target,
                                                  const time::TimeRange& range,
                                                  bool rippleAllTracks = false);

// --- Trimming ---------------------------------------------------------------

/// Move one edge of a clip. Neighbours stay put, so this opens or closes a gap.
[[nodiscard]] Result<CommandPtr> makeTrim(model::Project& project, const EditTarget& target,
                                          model::ClipId clip, Edge edge,
                                          const time::RationalTime& delta);

/// Move one edge and shift everything after it by the same amount, so no gap
/// opens and the rest of the cut keeps its relative timing.
[[nodiscard]] Result<CommandPtr> makeRippleTrim(model::Project& project, const EditTarget& target,
                                                model::ClipId clip, Edge edge,
                                                const time::RationalTime& delta,
                                                bool rippleAllTracks = false);

/// Move the cut between `clip` and the clip immediately after it. Total
/// duration is unchanged: one clip gains exactly what the other loses.
[[nodiscard]] Result<CommandPtr> makeRoll(model::Project& project, const EditTarget& target,
                                          model::ClipId clip, const time::RationalTime& delta);

/// Change which part of the source a clip shows without moving it. Nothing else
/// on the timeline is affected.
[[nodiscard]] Result<CommandPtr> makeSlip(model::Project& project, const EditTarget& target,
                                          model::ClipId clip, const time::RationalTime& delta);

/// Move a clip in time, with its neighbours absorbing the change. The clip's
/// content is untouched; the clips either side gain and lose duration.
[[nodiscard]] Result<CommandPtr> makeSlide(model::Project& project, const EditTarget& target,
                                           model::ClipId clip, const time::RationalTime& delta);

// --- Clip properties --------------------------------------------------------
//
// These change what a clip looks or sounds like rather than where it sits, so
// none of them can move a clip or collide with a neighbour. They exist as
// commands anyway, because the command stack is the only write path into the
// model -- an "obviously safe" mutation that bypassed it would be the one
// operation undo did not cover.
//
// Each carries a merge key, so dragging a slider is one undo step rather than
// one per pixel.

/// Position, scale, rotation, anchor and opacity.
[[nodiscard]] Result<CommandPtr> makeSetTransform(model::Project& project, const EditTarget& target,
                                                  model::ClipId clip,
                                                  const model::Transform& transform);

[[nodiscard]] Result<CommandPtr> makeSetBlendMode(model::Project& project, const EditTarget& target,
                                                  model::ClipId clip, model::BlendMode blend);

/// Clip gain in decibels and pan from -1 to +1.
[[nodiscard]] Result<CommandPtr> makeSetClipAudio(model::Project& project, const EditTarget& target,
                                                  model::ClipId clip, double gainDb, double pan);

/// Whether the clip contributes at all. Disabled clips stay on the timeline and
/// keep their place; they simply stop being composited or mixed.
[[nodiscard]] Result<CommandPtr> makeSetClipEnabled(model::Project& project,
                                                    const EditTarget& target, model::ClipId clip,
                                                    bool enabled);

// --- Transitions ------------------------------------------------------------

/// Add a cross dissolve across the cut nearest `at`.
///
/// The span straddles the cut, so both clips need material beyond it -- the
/// outgoing clip has to be readable past its out point and the incoming one
/// before its in point. A clip already using the whole of its source has no
/// such handles, and the dissolve is refused rather than silently shortened or
/// filled with black.
[[nodiscard]] Result<CommandPtr> makeAddCrossDissolve(model::Project& project,
                                                      const EditTarget& target,
                                                      const time::RationalTime& at,
                                                      const time::RationalTime& duration);

[[nodiscard]] Result<CommandPtr> makeRemoveTransition(model::Project& project,
                                                      const EditTarget& target,
                                                      model::TransitionId transition);

// --- Structure --------------------------------------------------------------

[[nodiscard]] Result<CommandPtr> makeAddTrack(model::Project& project, model::SequenceId sequence,
                                              model::TrackKind kind, std::string name);

[[nodiscard]] Result<CommandPtr> makeRemoveTrack(model::Project& project,
                                                 model::SequenceId sequence, model::TrackId track);

}  // namespace zaro::edit
