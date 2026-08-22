#pragma once

#include <string>
#include <utility>
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

// --- Captions ---------------------------------------------------------------

/// Replace a sequence's captions wholesale.
///
/// One operation rather than per-cue edits: captions arrive and leave as a
/// file, and an import that landed as three hundred undo steps would bury
/// whatever came before it.
[[nodiscard]] Result<CommandPtr> makeSetCaptions(model::Project& project,
                                                 model::SequenceId sequence,
                                                 const model::CaptionTrack& captions);

/// Place an adjustment layer: a clip that grades everything beneath it.
///
/// It has no media, so it needs no source to be found or opened -- which is
/// why it can be made from nothing but a track and a range.
[[nodiscard]] Result<CommandPtr> makeAddAdjustment(model::Project& project,
                                                   const EditTarget& target,
                                                   const time::TimeRange& range);

// --- Multicam ---------------------------------------------------------------

/// Place a multicam clip: several angles, one of them live.
///
/// The offsets are what sync the angles, and they are the caller's to work out
/// — from timecode, from a marker, or by ear. This places what it is given and
/// does not guess.
[[nodiscard]] Result<CommandPtr> makeMulticam(model::Project& project, const EditTarget& target,
                                              const std::vector<model::Clip::Angle>& angles,
                                              const time::TimeRange& range);

/// Cut to another angle at a moment.
///
/// A switch is a cut: the clip is split and the part after takes the new angle.
/// Modelling it as anything else would mean a second kind of edit that trims,
/// transitions and ripples all had to learn about — when what somebody wants is
/// exactly the cut they would have made by hand.
[[nodiscard]] Result<CommandPtr> makeSwitchAngle(model::Project& project, const EditTarget& target,
                                                 model::ClipId clip, std::int32_t angle,
                                                 const time::RationalTime& at);

/// Write worked-out offsets back onto a multicam clip's angles.
///
/// One command for all of them, because syncing is one decision: undoing it
/// halfway would leave a clip where two cameras agree and two do not, which is
/// worse than either state. Angles not named in `offsets` keep the offset they
/// have -- that is how a sync that could only do three of four cameras leaves
/// the fourth as it found it.
[[nodiscard]] Result<CommandPtr> makeSetAngleOffsets(
    model::Project& project, const EditTarget& target, model::ClipId clip,
    const std::vector<std::pair<std::int32_t, time::RationalTime>>& offsets);

/// Put one sequence on another's timeline.
///
/// Refused if it would make a cycle: a sequence containing itself, directly or
/// through any chain of nests, is a render that never finishes. Checked here
/// rather than guarded against while rendering, because a depth limit turns an
/// impossible project into a merely wrong one and explains nothing to whoever
/// made it.
[[nodiscard]] Result<CommandPtr> makeNestSequence(model::Project& project, const EditTarget& target,
                                                  model::SequenceId nested,
                                                  const time::RationalTime& at);

/// Place a generated shape on a track, at a time, for a duration.
///
/// A graphic has no media, so it has no source range to speak of -- but a clip
/// does, and every operation that trims or retimes one uses it. It gets a
/// source range identical to its timeline range, which makes a graphic behave
/// like a piece of media that happens to be exactly as long as it needs to be.
[[nodiscard]] Result<CommandPtr> makeAddGraphic(model::Project& project, const EditTarget& target,
                                                const model::Graphic& graphic,
                                                const time::TimeRange& range);

// --- Retiming ---------------------------------------------------------------

/// Change how fast a clip plays.
///
/// The source range is what it is: a retime changes how long the clip occupies
/// the timeline, not which frames it covers. `ripple` moves everything after it
/// so the cut stays closed — without that, speeding a clip up leaves a hole and
/// slowing it down runs over its neighbour.
[[nodiscard]] Result<CommandPtr> makeSetSpeed(model::Project& project, const EditTarget& target,
                                              model::ClipId clip, double speed, bool reversed,
                                              bool ripple = true);

// --- Time remapping ---------------------------------------------------------
//
// Constant speed is a property of the clip's two ranges; a *varying* speed is a
// curve, and it is a curve of which frame to show rather than of how fast to
// go. Storing the speed would mean integrating it to find a frame, and an
// integral accumulates its own error over a long clip -- so a freeze that was
// supposed to end on frame 300 ends on 299 or 302 depending how far into the
// timeline it sits. Storing the frame makes every keyframe exact by
// construction and leaves speed as the slope, which is the thing nobody has to
// be told twice.

/// Turn time remapping on for a clip, seeding the curve it already plays.
///
/// The seeded curve is the identity: two keyframes, one at each end of the
/// clip, holding exactly the mapping the clip has now. Switching remapping on
/// must not change the picture -- it is a statement about what can be edited
/// next, not an edit.
///
/// Passing false removes the curve, which puts the clip back on its ranges.
[[nodiscard]] Result<CommandPtr> makeSetTimeRemapped(model::Project& project,
                                                     const EditTarget& target, model::ClipId clip,
                                                     bool remapped);

/// Freeze a clip on the frame showing at a moment.
///
/// A remap whose value never changes, rather than a separate kind of clip:
/// everything that already works on a remapped clip -- the keyframe lane,
/// trimming, the render cache -- goes on working, and a freeze that somebody
/// then wants to ramp out of is two keyframes away rather than a different
/// feature.
[[nodiscard]] Result<CommandPtr> makeFreezeFrame(model::Project& project, const EditTarget& target,
                                                 model::ClipId clip, const time::RationalTime& at);

/// Set a clip's mask: where on the screen it shows through.
[[nodiscard]] Result<CommandPtr> makeSetMask(model::Project& project, const EditTarget& target,
                                             model::ClipId clip, const model::Mask& mask);

/// Change a graphic's shape, size, colour or feather.
[[nodiscard]] Result<CommandPtr> makeSetGraphic(model::Project& project, const EditTarget& target,
                                                model::ClipId clip, const model::Graphic& graphic);

/// The look LUT: which file, and how much of it.
[[nodiscard]] Result<CommandPtr> makeSetLut(model::Project& project, const EditTarget& target,
                                            model::ClipId clip, const model::LutRef& lut);

/// The secondary: its qualifier, its correction, and the mask view.
[[nodiscard]] Result<CommandPtr> makeSetSecondary(model::Project& project, const EditTarget& target,
                                                  model::ClipId clip,
                                                  const model::Secondary& secondary);

/// Tone curves: master and per-channel.
[[nodiscard]] Result<CommandPtr> makeSetToneCurves(model::Project& project,
                                                   const EditTarget& target, model::ClipId clip,
                                                   const model::ToneCurves& curves);

/// Point a clip at different media, keeping the cut.
///
/// The timeline range never moves: the whole reason to replace footage is that
/// the edit is right and the material is wrong -- a graded take swapped for the
/// ungraded one, a placeholder swapped for the delivery. Everything else on the
/// clip stays too, because the alternative is somebody re-doing a grade they
/// already did.
///
/// The source range keeps its offset where the new media is long enough, and
/// slides back to fit where it is not. Refused outright only when the new media
/// is shorter than the clip: at that point there is no honest answer, and
/// silently shortening the clip would ripple a cut somebody did not ask to
/// change.
[[nodiscard]] Result<CommandPtr> makeReplaceSource(model::Project& project,
                                                   const EditTarget& target, model::ClipId clip,
                                                   model::MediaRefId media);

/// The clip's effects, in order.
///
/// The whole list at once rather than add, remove and reorder as separate
/// operations: reordering is a move, a move is a remove and an insert, and
/// three commands that have to compose correctly are three chances to leave a
/// stack in a state no sequence of user actions could produce.
[[nodiscard]] Result<CommandPtr> makeSetEffects(model::Project& project, const EditTarget& target,
                                                model::ClipId clip,
                                                const std::vector<model::Effect>& effects);

/// The three colour wheels, as an ASC CDL.
[[nodiscard]] Result<CommandPtr> makeSetWheels(model::Project& project, const EditTarget& target,
                                               model::ClipId clip,
                                               const model::ColorWheels& wheels);

/// Darkening towards the corners.
[[nodiscard]] Result<CommandPtr> makeSetVignette(model::Project& project, const EditTarget& target,
                                                 model::ClipId clip,
                                                 const model::Vignette& vignette);

/// The keyer: what of the clip is transparent.
[[nodiscard]] Result<CommandPtr> makeSetKeyer(model::Project& project, const EditTarget& target,
                                              model::ClipId clip, const model::Keyer& keyer);

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
/// Replace a parameter's whole curve.
///
/// One command, because the curves this exists for are written by an analysis
/// rather than by hand: undoing an auto-duck should give back the level
/// somebody had, not remove two hundred keyframes one at a time. An empty
/// curve clears the parameter, which is how the same command undoes itself.
[[nodiscard]] Result<CommandPtr> makeSetCurve(model::Project& project, const EditTarget& target,
                                              model::ClipId clip, model::Param param,
                                              const model::Curve& curve);

/// Where a tracked mask goes: both offset curves at once.
///
/// One command rather than two, because a track produces one answer. Two
/// commands would leave an undo that moved the mask back horizontally and not
/// vertically, which is a state nothing asked for and nothing can draw
/// sensibly. Empty curves clear the track and put the mask back where it was
/// drawn.

[[nodiscard]] Result<CommandPtr> makeTrackMask(model::Project& project, const EditTarget& target,
                                               model::ClipId clip, const model::Curve& x,
                                               const model::Curve& y);

/// What the stabiliser found: both counter-movement curves and the zoom, in
/// one command. Three commands would leave undo able to stop somewhere that
/// holds the picture still and shows its edges. Empty curves and a zoom of one
/// clear the stabilisation.
[[nodiscard]] Result<CommandPtr> makeStabilise(model::Project& project, const EditTarget& target,
                                               model::ClipId clip, const model::Curve& x,
                                               const model::Curve& y, double zoom);

/// Write whatever somebody wants to remember about a file.
///
/// A command rather than a direct edit, so notes undo like everything else and
/// so a project with notes in it reads as modified -- the alternative is
/// somebody typing a note, quitting, and being told there was nothing to save.
[[nodiscard]] Result<CommandPtr> makeSetMediaNotes(model::Project& project, model::MediaRefId media,
                                                   const std::string& notes);

/// Point a media reference at a different file.
///
/// The digest is recomputed from the new file, so a relink that landed on the
/// right thing leaves the project able to find it again next time.
[[nodiscard]] Result<CommandPtr> makeRelinkMedia(model::Project& project, model::MediaRefId media,
                                                 const std::string& path);

/// Pin a clip to another, so it follows that clip's position and scale.
///
/// An invalid host unpins. Refuses a pin to itself, to a clip in another
/// sequence, or one that would close a loop: a cycle would be a picture whose
/// position is defined by its own position, and the renderer's depth limit is
/// a backstop for files that arrive with one rather than a licence to make
/// them here.
[[nodiscard]] Result<CommandPtr> makePinTo(model::Project& project, const EditTarget& target,
                                           model::ClipId clip, model::ClipId host);

/// Drop a graphic template onto the timeline at a length of its own choosing.
///
/// The template keeps everything about the graphic and gives up only where it
/// used to sit. Its responsive intro and outro come with it and are *not*
/// rescaled to the new length -- that is the point of them: a lower third
/// dropped in at half its original length still animates on and off at the
/// speed it was designed at.
[[nodiscard]] Result<CommandPtr> makePlaceGraphicTemplate(model::Project& project,
                                                          const EditTarget& target,
                                                          const model::Clip& templateClip,
                                                          const time::TimeRange& range);

/// Protect the first and last stretch of a clip's animation from being
/// stretched with the clip.
///
/// The clip's current duration is recorded as the authored length at the same
/// time, because that is what the intro and outro are measured against. Zero
/// for both turns it off, and the animation goes back to stretching with the
/// clip -- which is what a trim does to everything else on it.
[[nodiscard]] Result<CommandPtr> makeSetResponsive(model::Project& project,
                                                   const EditTarget& target, model::ClipId clip,
                                                   const time::RationalTime& intro,
                                                   const time::RationalTime& outro);

/// What a clip's sound is for.
[[nodiscard]] Result<CommandPtr> makeSetAudioRole(model::Project& project, const EditTarget& target,
                                                  model::ClipId clip, model::AudioRole role);

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

/// Say who left a marker and whether it has been dealt with.
///
/// Separate from `makeUpdateMarker` because these are the two fields a review
/// changes and the others are not: ticking a comment off should not be able to
/// rewrite what it said.
[[nodiscard]] Result<CommandPtr> makeSetMarkerReview(model::Project& project,
                                                     model::SequenceId sequence,
                                                     model::MarkerId marker, std::string author,
                                                     bool resolved);

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

/// Change an existing transition's kind and direction.
///
/// Separate from adding one, because that is how it is used: somebody drops a
/// dissolve on a cut and then decides it wants to be a wipe. Making them
/// re-add it would mean re-choosing the duration and re-finding the cut.
[[nodiscard]] Result<CommandPtr> makeSetTransitionKind(model::Project& project,
                                                       const EditTarget& target,
                                                       model::TransitionId transition,
                                                       model::TransitionKind kind,
                                                       model::TransitionDirection direction);

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

/// What a mixer strip controls.
struct TrackState {
    bool muted{false};
    bool soloed{false};
    double gainDb{0.0};
    double pan{0.0};
};

/// Set a track's mixer state. One operation rather than four, because a strip
/// is one thing and undoing a fader move should not also require undoing the
/// mute that was pressed with it.
[[nodiscard]] Result<CommandPtr> makeSetTrackState(model::Project& project,
                                                   model::SequenceId sequence, model::TrackId track,
                                                   const TrackState& state);

/// A track's processing chain.
[[nodiscard]] Result<CommandPtr> makeSetTrackProcessing(model::Project& project,
                                                        model::SequenceId sequence,
                                                        model::TrackId track,
                                                        const model::AudioEq& eq,
                                                        const model::Compressor& compressor);

[[nodiscard]] Result<CommandPtr> makeAddTrack(model::Project& project, model::SequenceId sequence,
                                              model::TrackKind kind, std::string name);

[[nodiscard]] Result<CommandPtr> makeRemoveTrack(model::Project& project,
                                                 model::SequenceId sequence, model::TrackId track);

/// Cut a track at several points at once.
///
/// One command rather than a razor per point, because detecting the cuts in a
/// shot is one decision: undoing it should give back the clip somebody had, not
/// peel the cuts off one at a time in an order they never chose.
///
/// Points that fall outside a clip, or on a cut that already exists, are
/// skipped rather than refused. The list comes from an analysis of the picture,
/// and rejecting all of it because one point landed in a gap would throw away
/// the answer over the least interesting part of it.
[[nodiscard]] Result<CommandPtr> makeRazorAt(model::Project& project, const EditTarget& target,
                                             const std::vector<time::RationalTime>& points);

/// What the sequence is delivered as: its display curve and where its
/// highlights start rolling off.
///
/// A command like any other, because it changes what every frame of the
/// sequence looks like coming out -- and because the curve editor and the
/// scopes are drawn against it, so it changes what a grade is judged on too.
[[nodiscard]] Result<CommandPtr> makeSetSequenceOutput(model::Project& project,
                                                       model::SequenceId sequence,
                                                       const model::Sequence::Output& output);

/// Set an empty sequence's rate and frame size.
///
/// **Refused once anything is on it.** Every clip's timeline range is expressed
/// at the sequence's rate, so changing it under a cut would retime the whole
/// thing -- silently, and by an amount that depends on the ratio between two
/// rates nobody was thinking about. An empty sequence has nothing to disagree
/// with, which is the only moment this is safe and also the only moment it is
/// useful.
///
/// This is what lets a new project take its format from the first thing put on
/// its timeline instead of from a dialog asked before there was any footage to
/// answer it with.
[[nodiscard]] Result<CommandPtr> makeConformSequence(model::Project& project,
                                                     model::SequenceId sequence,
                                                     const time::Rational& frameRate,
                                                     std::int32_t width, std::int32_t height);

}  // namespace zaro::edit
