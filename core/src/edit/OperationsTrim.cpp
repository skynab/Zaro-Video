// Moving an edge: trim, ripple trim, roll, slip and slide.
//
// One family, because they differ only in which edges move and what is
// expected to absorb the difference.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "zaro/core/Check.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/core/media/Waveform.h"

#include "OperationsCommon.h"

namespace zaro::edit {
namespace {

using model::Clip;
using model::ClipId;
using model::Project;
using model::Sequence;
using model::Track;
using model::TrackId;
using time::RationalTime;
using time::TimeRange;

// The helpers more than one operation file needs; see OperationsCommon.h.
using detail::atRate;
using detail::checkSourceFits;
using detail::idText;
using detail::linkedGroup;
using detail::locate;
using detail::makeCommand;
using detail::requireClip;
using detail::trimmedIn;
using detail::trimmedOut;

/// Replace a clip and shift everything after it in one rebuild.
///
/// Doing it as two steps -- replace then shift, or shift then replace -- puts
/// the track through a state where two clips overlap for one of the two
/// directions, whichever order is chosen. There is no correct order, so the
/// answer is not to have an intermediate state at all.
void replaceAndRipple(Track& track, ClipId id, const Clip& replacement, const RationalTime& from,
                      const RationalTime& delta) {
    std::vector<Clip> rebuilt;
    rebuilt.reserve(track.clips().size());
    for (const Clip& clip : track.clips()) {
        if (clip.id == id) {
            rebuilt.push_back(replacement);
        } else if (clip.start() >= from) {
            Clip moved = clip;
            moved.timelineRange = TimeRange{clip.start() + delta, clip.duration()};
            rebuilt.push_back(std::move(moved));
        } else {
            rebuilt.push_back(clip);
        }
    }
    std::sort(rebuilt.begin(), rebuilt.end(),
              [](const Clip& a, const Clip& b) { return a.start() < b.start(); });
    track.setClips(std::move(rebuilt));
}

/// Whether every track a ripple would touch can absorb it.
/// Whether the tracks carried along by a ripple can absorb it.
///
/// Only the *other* tracks are checked. The track being edited absorbs its own
/// change in the same rebuild that applies it, so measuring it against the
/// pre-edit layout would reject perfectly good shrinks.
Status checkRippleFits(Sequence& sequence, const Track& edited, const RationalTime& from,
                       const RationalTime& delta) {
    for (const model::TrackKind kind : {model::TrackKind::Video, model::TrackKind::Audio}) {
        for (const Track& other : sequence.tracksMutable(kind)) {
            if (other.isLocked() || !other.isSyncLocked() || other.id() == edited.id()) {
                continue;
            }
            if (!other.canShiftFrom(from, delta)) {
                return Error{ErrorCode::InvalidData,
                             "the ripple would run track '" + other.name() + "' into itself"};
            }
        }
    }
    return {};
}

/// Shared shape of every trim: work out the new clip, validate it against the
/// source and its neighbours, and hand back both for the caller to install.
struct TrimPlan {
    Clip result;
    RationalTime durationChange;
};

Result<TrimPlan> planTrim(const Project& project, const Sequence& sequence, const Track& track,
                          const Clip& clip, Edge edge, const RationalTime& rawDelta) {
    const RationalTime delta = atRate(rawDelta, sequence.frameRate());
    if (delta.isZero()) {
        return Error{ErrorCode::InvalidData, "trim of zero frames"};
    }

    Clip result = edge == Edge::In ? trimmedIn(clip, clip.start() + delta)
                                   : trimmedOut(clip, clip.endExclusive() + delta);

    if (result.timelineRange.isEmpty() || result.duration().frames() <= 0) {
        return Error{ErrorCode::InvalidData, "trim would leave nothing of the clip"};
    }
    if (result.start().frames() < 0) {
        return Error{ErrorCode::InvalidData, "trim would start the clip before the sequence"};
    }
    if (Status fits = checkSourceFits(project, result); !fits) {
        return fits.error();
    }
    if (!track.isRangeFree(result.timelineRange, clip.id)) {
        return Error{ErrorCode::InvalidData, "trim would run into the neighbouring clip"};
    }

    return TrimPlan{std::move(result), result.duration() - clip.duration()};
}

}  // namespace

Result<CommandPtr> makeTrim(Project& project, const EditTarget& target, ClipId clipId, Edge edge,
                            const RationalTime& delta) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    auto found = requireClip(*located->track, clipId);
    if (!found) {
        return found.error();
    }
    auto plan = planTrim(project, *located->sequence, *located->track, **found, edge, delta);
    if (!plan) {
        return plan.error();
    }

    const TrackId trackId = target.track;
    const RationalTime step = atRate(delta, located->sequence->frameRate());

    return makeCommand(
        target.sequence, edge == Edge::In ? "Trim in" : "Trim out",
        "trim:" + idText(clipId) + (edge == Edge::In ? ":in" : ":out"),
        [result = plan->result, clipId, trackId, edge, step](Sequence& sequence) {
            Track* track = sequence.findTrack(trackId);
            ZARO_CHECK(track != nullptr, "track vanished between build and apply");

            // Linked clips take the same trim. One that cannot -- because it
            // would run out of source or into a neighbour -- is left alone
            // rather than blocking the edit on the clip actually being dragged.
            for (const auto& [otherTrack, otherClip] : linkedGroup(sequence, trackId, clipId)) {
                if (otherClip == clipId) {
                    continue;
                }
                Track* other = sequence.findTrack(otherTrack);
                if (other == nullptr) {
                    continue;
                }
                const Clip* existing = other->find(otherClip);
                if (existing == nullptr) {
                    continue;
                }
                const Clip trimmed = edge == Edge::In
                                         ? trimmedIn(*existing, existing->start() + step)
                                         : trimmedOut(*existing, existing->endExclusive() + step);
                if (trimmed.duration().frames() <= 0 || trimmed.start().frames() < 0) {
                    continue;
                }
                if (!other->isRangeFree(trimmed.timelineRange, otherClip)) {
                    continue;
                }
                other->replace(otherClip, trimmed);
            }

            track->replace(clipId, result);
        });
}

Result<CommandPtr> makeRippleTrim(Project& project, const EditTarget& target, ClipId clipId,
                                  Edge edge, const RationalTime& delta, bool rippleAllTracks) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    auto found = requireClip(*located->track, clipId);
    if (!found) {
        return found.error();
    }
    const Clip original = **found;
    const RationalTime step = atRate(delta, located->sequence->frameRate());

    // A ripple trim of the in point does not move the clip: the gap it would
    // have left is closed by pulling everything after it back instead.
    Clip result = original;
    if (edge == Edge::In) {
        // Check the resulting length before building the range: a TimeRange
        // cannot hold a negative duration, so an over-long trim has to be
        // refused here rather than caught after the fact.
        const RationalTime newDuration = original.duration() - step;
        if (newDuration.frames() <= 0) {
            return Error{ErrorCode::InvalidData, "trim would leave nothing of the clip"};
        }
        result.sourceRange = TimeRange::fromStartEnd(original.sourceTimeAt(original.start() + step),
                                                     original.sourceRange.endExclusive());
        result.timelineRange = TimeRange{original.start(), newDuration};
    } else {
        result = trimmedOut(original, original.endExclusive() + step);
    }

    if (result.timelineRange.isEmpty() || result.duration().frames() <= 0) {
        return Error{ErrorCode::InvalidData, "trim would leave nothing of the clip"};
    }
    if (Status fits = checkSourceFits(project, result); !fits) {
        return fits.error();
    }

    const RationalTime change = result.duration() - original.duration();
    const RationalTime shiftFrom = original.endExclusive();
    const TrackId trackId = target.track;

    if (rippleAllTracks) {
        if (Status fits = checkRippleFits(*located->sequence, *located->track, shiftFrom, change);
            !fits) {
            return fits.error();
        }
    }

    return makeCommand(
        target.sequence, edge == Edge::In ? "Ripple trim in" : "Ripple trim out",
        "rippletrim:" + idText(clipId) + (edge == Edge::In ? ":in" : ":out"),
        [result, clipId, trackId, change, shiftFrom, rippleAllTracks](Sequence& sequence) {
            Track* track = sequence.findTrack(trackId);
            ZARO_CHECK(track != nullptr, "track vanished between build and apply");

            if (rippleAllTracks) {
                for (const model::TrackKind kind :
                     {model::TrackKind::Video, model::TrackKind::Audio}) {
                    for (Track& other : sequence.tracksMutable(kind)) {
                        if (!other.isLocked() && other.id() != trackId) {
                            other.shiftFrom(shiftFrom, change);
                        }
                    }
                }
            }
            replaceAndRipple(*track, clipId, result, shiftFrom, change);
        });
}

Result<CommandPtr> makeRoll(Project& project, const EditTarget& target, ClipId clipId,
                            const RationalTime& delta) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    auto found = requireClip(*located->track, clipId);
    if (!found) {
        return found.error();
    }
    const Clip left = **found;

    const auto index = located->track->indexOf(clipId);
    if (!index || *index + 1 >= located->track->clips().size()) {
        return Error{ErrorCode::NotFound, "there is no clip after this one to roll against"};
    }
    const Clip right = located->track->clips()[*index + 1];
    if (right.start() != left.endExclusive()) {
        return Error{ErrorCode::InvalidData, "roll needs two clips that meet; there is a gap"};
    }

    const RationalTime step = atRate(delta, located->sequence->frameRate());
    if (step.isZero()) {
        return Error{ErrorCode::InvalidData, "roll of zero frames"};
    }

    const Clip newLeft = trimmedOut(left, left.endExclusive() + step);
    const Clip newRight = trimmedIn(right, right.start() + step);

    if (newLeft.duration().frames() <= 0 || newRight.duration().frames() <= 0) {
        return Error{ErrorCode::InvalidData, "roll would consume one of the clips entirely"};
    }
    if (Status fits = checkSourceFits(project, newLeft); !fits) {
        return Error{fits.error().code(), "left of the cut: " + fits.error().message()};
    }
    if (Status fits = checkSourceFits(project, newRight); !fits) {
        return Error{fits.error().code(), "right of the cut: " + fits.error().message()};
    }

    const TrackId trackId = target.track;
    return makeCommand(target.sequence, "Roll", "roll:" + idText(clipId),
                       [newLeft, newRight, trackId](Sequence& sequence) {
                           Track* track = sequence.findTrack(trackId);
                           ZARO_CHECK(track != nullptr, "track vanished between build and apply");
                           // Remove both before reinserting: whichever is
                           // rewritten first would otherwise overlap the other.
                           track->remove(newLeft.id);
                           track->remove(newRight.id);
                           track->insert(newLeft);
                           track->insert(newRight);
                       });
}

Result<CommandPtr> makeSlip(Project& project, const EditTarget& target, ClipId clipId,
                            const RationalTime& delta) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    auto found = requireClip(*located->track, clipId);
    if (!found) {
        return found.error();
    }
    const Clip original = **found;
    const RationalTime step = atRate(delta, located->sequence->frameRate())
                                  .rescaledTo(original.sourceRange.start().rate());

    Clip result = original;
    result.sourceRange =
        TimeRange{original.sourceRange.start() + step, original.sourceRange.duration()};

    if (Status fits = checkSourceFits(project, result); !fits) {
        return Error{fits.error().code(), "slip ran out of source: " + fits.error().message()};
    }

    const TrackId trackId = target.track;
    return makeCommand(target.sequence, "Slip", "slip:" + idText(clipId),
                       [result, clipId, trackId](Sequence& sequence) {
                           Track* track = sequence.findTrack(trackId);
                           ZARO_CHECK(track != nullptr, "track vanished between build and apply");
                           track->replace(clipId, result);
                       });
}

Result<CommandPtr> makeSlide(Project& project, const EditTarget& target, ClipId clipId,
                             const RationalTime& delta) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    const auto index = located->track->indexOf(clipId);
    if (!index) {
        return Error{ErrorCode::NotFound, "clip " + idText(clipId) + " is not on this track"};
    }
    if (*index == 0 || *index + 1 >= located->track->clips().size()) {
        return Error{ErrorCode::InvalidData, "slide needs a clip on each side"};
    }

    const std::vector<Clip>& clips = located->track->clips();
    const Clip before = clips[*index - 1];
    const Clip middle = clips[*index];
    const Clip after = clips[*index + 1];

    if (before.endExclusive() != middle.start() || middle.endExclusive() != after.start()) {
        return Error{ErrorCode::InvalidData, "slide needs three clips that meet; there is a gap"};
    }

    const RationalTime step = atRate(delta, located->sequence->frameRate());
    if (step.isZero()) {
        return Error{ErrorCode::InvalidData, "slide of zero frames"};
    }

    Clip newMiddle = middle;
    newMiddle.timelineRange = TimeRange{middle.start() + step, middle.duration()};
    const Clip newBefore = trimmedOut(before, before.endExclusive() + step);
    const Clip newAfter = trimmedIn(after, after.start() + step);

    if (newBefore.duration().frames() <= 0 || newAfter.duration().frames() <= 0) {
        return Error{ErrorCode::InvalidData, "slide would consume a neighbouring clip"};
    }
    if (Status fits = checkSourceFits(project, newBefore); !fits) {
        return Error{fits.error().code(), "clip before: " + fits.error().message()};
    }
    if (Status fits = checkSourceFits(project, newAfter); !fits) {
        return Error{fits.error().code(), "clip after: " + fits.error().message()};
    }

    const TrackId trackId = target.track;
    return makeCommand(target.sequence, "Slide", "slide:" + idText(clipId),
                       [newBefore, newMiddle, newAfter, trackId](Sequence& sequence) {
                           Track* track = sequence.findTrack(trackId);
                           ZARO_CHECK(track != nullptr, "track vanished between build and apply");
                           track->remove(newBefore.id);
                           track->remove(newMiddle.id);
                           track->remove(newAfter.id);
                           track->insert(newBefore);
                           track->insert(newMiddle);
                           track->insert(newAfter);
                       });
}

}  // namespace zaro::edit
