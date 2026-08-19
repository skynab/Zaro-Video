#pragma once

#include <string>
#include <vector>

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

/// Which clip, on which track.
struct ClipRef {
    model::TrackId track;
    model::ClipId clip;
};

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

/// Three-point editing: two points marked in the source, one on the timeline,
/// and the fourth derived.
///
/// This is how an edit is actually assembled — mark the part of the take you
/// want, put the playhead where it goes, and press a key. The duration follows
/// from the source range rather than being chosen separately, which is the
/// whole point of counting to three.
enum class PlaceMode {
    Overwrite,  ///< Replace whatever is under it.
    Insert,     ///< Push what follows to the right.
};

/// Build a clip from a media reference and a range of it, and place it.
///
/// The source range is in the media's own frame rate and the placement is in
/// the sequence's; converting between them is the part worth having in one
/// tested place rather than in whichever panel happens to be assembling a clip.
[[nodiscard]] Result<CommandPtr> makePlaceFromSource(
    model::Project& project, const EditTarget& target, model::MediaRefId media,
    const time::TimeRange& sourceRange, const time::RationalTime& timelineStart, PlaceMode mode);

// --- Several clips at once --------------------------------------------------

/// Move a set of clips by the same amount, keeping their spacing.
///
/// Not the same as moving each in turn: doing it one at a time would have each
/// overwrite the next while they are mid-flight, and the result would depend on
/// the order they happened to be in. They are all lifted first and then placed.
[[nodiscard]] Result<CommandPtr> makeMoveClips(model::Project& project, model::SequenceId sequence,
                                               const std::vector<ClipRef>& clips,
                                               const time::RationalTime& delta);

/// Remove a set of clips, leaving gaps or closing them.
[[nodiscard]] Result<CommandPtr> makeRemoveClips(model::Project& project,
                                                 model::SequenceId sequence,
                                                 const std::vector<ClipRef>& clips, bool ripple);

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

/// The secondary: its qualifier, its correction, and the mask view.
[[nodiscard]] Result<CommandPtr> makeSetSecondary(model::Project& project, const EditTarget& target,
                                                  model::ClipId clip,
                                                  const model::Secondary& secondary);

/// Tone curves: master and per-channel.
[[nodiscard]] Result<CommandPtr> makeSetToneCurves(model::Project& project,
                                                   const EditTarget& target, model::ClipId clip,
                                                   const model::ToneCurves& curves);

/// Primary colour correction: white balance, exposure, contrast, saturation.
[[nodiscard]] Result<CommandPtr> makeSetColorCorrection(model::Project& project,
                                                        const EditTarget& target,
                                                        model::ClipId clip,
                                                        const model::ColorCorrection& color);

// --- Keyframes --------------------------------------------------------------
//
// Keyframe times are in the clip's source time, the same as the model stores
// them (ADR-008). The panel and the timeline both work in sequence time and
// convert at the edge, because a keyframe's position on screen is a question
// about where the clip currently sits and its identity is not.

/// Add a keyframe, or replace the one already at that time.
[[nodiscard]] Result<CommandPtr> makeSetKeyframe(
    model::Project& project, const EditTarget& target, model::ClipId clip, model::Param param,
    const time::RationalTime& sourceTime, double value,
    model::Interpolation interpolation = model::Interpolation::Linear);

[[nodiscard]] Result<CommandPtr> makeRemoveKeyframe(model::Project& project,
                                                    const EditTarget& target, model::ClipId clip,
                                                    model::Param param,
                                                    const time::RationalTime& sourceTime);

/// Move a keyframe in time, keeping its value and shape.
[[nodiscard]] Result<CommandPtr> makeMoveKeyframe(model::Project& project, const EditTarget& target,
                                                  model::ClipId clip, model::Param param,
                                                  const time::RationalTime& from,
                                                  const time::RationalTime& to);

[[nodiscard]] Result<CommandPtr> makeSetKeyframeInterpolation(
    model::Project& project, const EditTarget& target, model::ClipId clip, model::Param param,
    const time::RationalTime& sourceTime, model::Interpolation interpolation);

/// Move every keyframe at one instant, across all parameters.
///
/// The timeline draws one diamond per instant rather than one per parameter:
/// several parameters keyed together are one decision, and drawing them stacked
/// would put eight identical diamonds in a lane four pixels tall. Dragging that
/// diamond has to move all of them, and as one command, or undo would take
/// eight presses to put back what one drag moved.
[[nodiscard]] Result<CommandPtr> makeMoveKeyframesAt(model::Project& project,
                                                     const EditTarget& target, model::ClipId clip,
                                                     const time::RationalTime& from,
                                                     const time::RationalTime& to);

[[nodiscard]] Result<CommandPtr> makeRemoveKeyframesAt(model::Project& project,
                                                       const EditTarget& target, model::ClipId clip,
                                                       const time::RationalTime& sourceTime);

/// The stopwatch: start or stop animating one parameter.
///
/// Starting drops a keyframe at `timelineTime` holding the value the parameter
/// already had, so turning animation on never changes the picture. Stopping
/// keeps the value the parameter has *at that moment* as the new static value,
/// rather than reverting to whatever it was before it was animated — which
/// would make the picture jump at the instant the user turned animation off.
[[nodiscard]] Result<CommandPtr> makeSetParameterAnimated(model::Project& project,
                                                          const EditTarget& target,
                                                          model::ClipId clip, model::Param param,
                                                          bool animated,
                                                          const time::RationalTime& timelineTime);

/// Clip gain in decibels and pan from -1 to +1.
[[nodiscard]] Result<CommandPtr> makeSetClipAudio(model::Project& project, const EditTarget& target,
                                                  model::ClipId clip, double gainDb, double pan);

/// Whether the clip contributes at all. Disabled clips stay on the timeline and
/// keep their place; they simply stop being composited or mixed.
[[nodiscard]] Result<CommandPtr> makeSetClipEnabled(model::Project& project,
                                                    const EditTarget& target, model::ClipId clip,
                                                    bool enabled);

// --- Markers ----------------------------------------------------------------

/// Add a marker. A duration of zero becomes a one-frame point marker.
[[nodiscard]] Result<CommandPtr> makeAddMarker(model::Project& project, model::SequenceId sequence,
                                               const time::RationalTime& at,
                                               const time::RationalTime& duration, std::string name,
                                               std::int32_t colour = 0);

[[nodiscard]] Result<CommandPtr> makeRemoveMarker(model::Project& project,
                                                  model::SequenceId sequence,
                                                  model::MarkerId marker);

/// Rename a marker, or change its note or colour.
[[nodiscard]] Result<CommandPtr> makeUpdateMarker(model::Project& project,
                                                  model::SequenceId sequence,
                                                  model::MarkerId marker, std::string name,
                                                  std::string note, std::int32_t colour);

// --- Linking ----------------------------------------------------------------

/// Join clips into a link group, so they move, trim and are removed together.
///
/// Picture and its sound arrive together and should stay together: dragging one
/// and leaving the other is how a cut goes out of sync without anyone noticing.
[[nodiscard]] Result<CommandPtr> makeLinkClips(model::Project& project, model::SequenceId sequence,
                                               const std::vector<ClipRef>& clips);

/// Break a link group, leaving every clip in it standing alone.
[[nodiscard]] Result<CommandPtr> makeUnlinkClips(model::Project& project, const EditTarget& target,
                                                 model::ClipId clip);

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

// --- The project ------------------------------------------------------------

/// Add a media reference to the project.
///
/// The caller supplies an already-probed MediaRef, because probing touches the
/// file system and belongs on whichever thread the caller chooses rather than
/// inside a command.
[[nodiscard]] Result<CommandPtr> makeImportMedia(model::Project& project, model::MediaRef media);

// --- Structure --------------------------------------------------------------

[[nodiscard]] Result<CommandPtr> makeAddTrack(model::Project& project, model::SequenceId sequence,
                                              model::TrackKind kind, std::string name);

[[nodiscard]] Result<CommandPtr> makeRemoveTrack(model::Project& project,
                                                 model::SequenceId sequence, model::TrackId track);

}  // namespace zaro::edit
