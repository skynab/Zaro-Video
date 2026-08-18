#include "zaro/core/edit/Operations.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "zaro/core/Check.h"

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

/// A command whose edit is supplied as a lambda. The operations below differ
/// only in what they do to the sequence, so they share one command type rather
/// than each defining a class that would carry no extra state.
class LambdaCommand final : public SequenceCommand {
public:
    using Body = std::function<void(Sequence&)>;

    LambdaCommand(model::SequenceId sequence, std::string description, std::string mergeKey,
                  Body body)
        : SequenceCommand{sequence, std::move(description), std::move(mergeKey)},
          body_{std::move(body)} {}

protected:
    void mutate(Sequence& sequence) override { body_(sequence); }

private:
    Body body_;
};

CommandPtr makeCommand(model::SequenceId sequence, std::string description, std::string mergeKey,
                       LambdaCommand::Body body) {
    return std::make_unique<LambdaCommand>(sequence, std::move(description), std::move(mergeKey),
                                           std::move(body));
}

std::string idText(ClipId id) {
    return std::to_string(id.value());
}

// --- Lookups ----------------------------------------------------------------

struct Located {
    Sequence* sequence;
    Track* track;
};

Result<Located> locate(Project& project, const EditTarget& target) {
    Sequence* sequence = project.findSequence(target.sequence);
    if (sequence == nullptr) {
        return Error{ErrorCode::NotFound, "no such sequence"};
    }
    Track* track = sequence->findTrack(target.track);
    if (track == nullptr) {
        return Error{ErrorCode::NotFound, "no such track in this sequence"};
    }
    if (track->isLocked()) {
        return Error{ErrorCode::Unsupported, "track '" + track->name() + "' is locked"};
    }
    return Located{sequence, track};
}

Result<const Clip*> requireClip(const Track& track, ClipId id) {
    const Clip* clip = track.find(id);
    if (clip == nullptr) {
        return Error{ErrorCode::NotFound,
                     "clip " + idText(id) + " is not on track '" + track.name() + "'"};
    }
    return clip;
}

// --- Clip geometry ----------------------------------------------------------

/// Move a clip's in edge to `newStart`, taking the source range with it.
/// Rate differences and non-unit speeds are handled by sourceTimeAt, so this
/// stays correct for rate-converted material.
Clip trimmedIn(const Clip& clip, const RationalTime& newStart) {
    Clip out = clip;
    out.sourceRange =
        TimeRange::fromStartEnd(clip.sourceTimeAt(newStart), clip.sourceRange.endExclusive());
    out.timelineRange = TimeRange::fromStartEnd(newStart, clip.endExclusive());
    return out;
}

Clip trimmedOut(const Clip& clip, const RationalTime& newEnd) {
    Clip out = clip;
    out.sourceRange = TimeRange::fromStartEnd(clip.sourceRange.start(), clip.sourceTimeAt(newEnd));
    out.timelineRange = TimeRange::fromStartEnd(clip.start(), newEnd);
    return out;
}

/// The range of the source that actually exists, if we know it. Unprobed media
/// returns nothing, and then trims are not constrained -- refusing to edit
/// footage we have not looked at yet would be worse than allowing a trim that a
/// later probe reveals to be too long.
std::optional<TimeRange> availableSource(const Project& project, const Clip& clip) {
    const model::MediaRef* ref = project.findMedia(clip.source);
    if (ref == nullptr || !ref->info.duration.isPositive()) {
        return std::nullopt;
    }
    const time::Rational& rate = clip.sourceRange.start().rate();
    return TimeRange{RationalTime{0, rate}, RationalTime::fromSeconds(ref->info.duration, rate)};
}

Status checkSourceFits(const Project& project, const Clip& clip) {
    if (clip.timelineRange.isEmpty()) {
        return Error{ErrorCode::InvalidData, "a clip cannot have zero duration"};
    }
    if (clip.sourceRange.isEmpty()) {
        return Error{ErrorCode::InvalidData, "a clip cannot reference zero source"};
    }
    const auto available = availableSource(project, clip);
    if (!available) {
        return {};
    }
    if (clip.sourceRange.start() < available->start()) {
        return Error{ErrorCode::InvalidData, "trim runs off the start of the source media"};
    }
    if (clip.sourceRange.endExclusive() > available->endExclusive()) {
        return Error{ErrorCode::InvalidData, "trim runs past the end of the source media"};
    }
    return {};
}

// --- Range clearing ---------------------------------------------------------

/// Cut a hole in a track. Clips fully inside vanish, partly covered clips are
/// trimmed, and a clip spanning the whole hole is split around it.
void clearRange(Track& track, const TimeRange& range, model::IdGenerator& ids) {
    if (range.isEmpty()) {
        return;
    }
    std::vector<Clip> kept;
    kept.reserve(track.clips().size() + 1);

    for (const Clip& clip : track.clips()) {
        if (!clip.timelineRange.overlaps(range)) {
            kept.push_back(clip);
            continue;
        }
        const bool coversStart = clip.start() < range.start();
        const bool coversEnd = clip.endExclusive() > range.endExclusive();

        if (coversStart) {
            kept.push_back(trimmedOut(clip, range.start()));
        }
        if (coversEnd) {
            Clip tail = trimmedIn(clip, range.endExclusive());
            // The tail is a new clip: it has its own identity from here on, and
            // reusing the original's id would make two clips answer to it.
            if (coversStart) {
                tail.id = ids.next<model::ClipTag>();
            }
            kept.push_back(std::move(tail));
        }
        // Neither: the clip is wholly inside the hole and is dropped.
    }
    track.setClips(std::move(kept));
}

/// Shift a point onward, on one track or all of them.
void rippleFrom(Sequence& sequence, Track& track, const RationalTime& from,
                const RationalTime& delta, bool allTracks) {
    if (!allTracks) {
        track.shiftFrom(from, delta);
        return;
    }
    for (const model::TrackKind kind : {model::TrackKind::Video, model::TrackKind::Audio}) {
        for (Track& other : sequence.tracksMutable(kind)) {
            if (!other.isLocked()) {
                other.shiftFrom(from, delta);
            }
        }
    }
}

RationalTime atRate(const RationalTime& t, const time::Rational& rate) {
    return t.rescaledTo(rate);
}

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
            if (other.isLocked() || other.id() == edited.id()) {
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

}  // namespace

// --- Placement --------------------------------------------------------------

Result<CommandPtr> makeOverwrite(Project& project, const EditTarget& target, Clip clip) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    if (!clip.id.isValid()) {
        clip.id = project.ids().next<model::ClipTag>();
    }
    if (Status fits = checkSourceFits(project, clip); !fits) {
        return fits.error();
    }

    const TimeRange range = clip.timelineRange;
    model::IdGenerator& ids = project.ids();
    const TrackId trackId = target.track;

    return makeCommand(target.sequence, "Overwrite", {},
                       [clip = std::move(clip), range, trackId, &ids](Sequence& sequence) {
                           Track* track = sequence.findTrack(trackId);
                           ZARO_CHECK(track != nullptr, "track vanished between build and apply");
                           clearRange(*track, range, ids);
                           track->insert(clip);
                       });
}

Result<CommandPtr> makeInsert(Project& project, const EditTarget& target, Clip clip,
                              bool rippleAllTracks) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    if (!clip.id.isValid()) {
        clip.id = project.ids().next<model::ClipTag>();
    }
    if (Status fits = checkSourceFits(project, clip); !fits) {
        return fits.error();
    }

    const RationalTime at = clip.start();
    const RationalTime shift = clip.duration();
    model::IdGenerator& ids = project.ids();
    const TrackId trackId = target.track;

    return makeCommand(
        target.sequence, "Insert", {},
        [clip = std::move(clip), at, shift, trackId, rippleAllTracks, &ids](Sequence& sequence) {
            Track* track = sequence.findTrack(trackId);
            ZARO_CHECK(track != nullptr, "track vanished between build and apply");

            // Split anything straddling the insertion point before shifting, so
            // the two halves end up on opposite sides of the new material
            // rather than the whole clip jumping to one side.
            const auto splitStraddling = [&at, &ids](Track& t) {
                const Clip* straddling = t.clipAt(at);
                if (straddling == nullptr || straddling->start() == at) {
                    return;
                }
                const Clip original = *straddling;
                std::vector<Clip> rebuilt;
                for (const Clip& existing : t.clips()) {
                    if (existing.id != original.id) {
                        rebuilt.push_back(existing);
                        continue;
                    }
                    rebuilt.push_back(trimmedOut(original, at));
                    Clip tail = trimmedIn(original, at);
                    tail.id = ids.next<model::ClipTag>();
                    rebuilt.push_back(std::move(tail));
                }
                t.setClips(std::move(rebuilt));
            };

            if (rippleAllTracks) {
                for (const model::TrackKind kind :
                     {model::TrackKind::Video, model::TrackKind::Audio}) {
                    for (Track& other : sequence.tracksMutable(kind)) {
                        if (!other.isLocked()) {
                            splitStraddling(other);
                        }
                    }
                }
            } else {
                splitStraddling(*track);
            }

            rippleFrom(sequence, *track, at, shift, rippleAllTracks);
            track->insert(clip);
        });
}

Result<CommandPtr> makeMove(Project& project, const EditTarget& target, ClipId clipId,
                            TrackId toTrack, const RationalTime& newStart) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    auto found = requireClip(*located->track, clipId);
    if (!found) {
        return found.error();
    }
    if (located->sequence->findTrack(toTrack) == nullptr) {
        return Error{ErrorCode::NotFound, "no such destination track"};
    }

    const time::Rational& rate = located->sequence->frameRate();
    const RationalTime start = atRate(newStart, rate);
    if (start.frames() < 0) {
        return Error{ErrorCode::InvalidData, "a clip cannot start before the sequence does"};
    }

    Clip moved = **found;
    moved.timelineRange = TimeRange{start, moved.duration()};

    model::IdGenerator& ids = project.ids();
    const TrackId fromTrack = target.track;

    return makeCommand(target.sequence, "Move clip", "move:" + idText(clipId),
                       [moved, clipId, fromTrack, toTrack, &ids](Sequence& sequence) {
                           Track* source = sequence.findTrack(fromTrack);
                           Track* destination = sequence.findTrack(toTrack);
                           ZARO_CHECK(source != nullptr && destination != nullptr,
                                      "track vanished between build and apply");
                           source->remove(clipId);
                           clearRange(*destination, moved.timelineRange, ids);
                           destination->insert(moved);
                       });
}

// --- Cutting ----------------------------------------------------------------

Result<CommandPtr> makeRazor(Project& project, const EditTarget& target, const RationalTime& at) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    const RationalTime cut = atRate(at, located->sequence->frameRate());
    const Clip* clip = located->track->clipAt(cut);
    if (clip == nullptr) {
        return Error{ErrorCode::NotFound, "nothing to cut at that point"};
    }
    if (clip->start() == cut) {
        return Error{ErrorCode::InvalidData, "there is already a cut here"};
    }

    const ClipId originalId = clip->id;
    const ClipId tailId = project.ids().next<model::ClipTag>();
    const TrackId trackId = target.track;

    return makeCommand(target.sequence, "Razor", {},
                       [cut, originalId, tailId, trackId](Sequence& sequence) {
                           Track* track = sequence.findTrack(trackId);
                           ZARO_CHECK(track != nullptr, "track vanished between build and apply");
                           const Clip* target = track->find(originalId);
                           ZARO_CHECK(target != nullptr, "clip vanished between build and apply");

                           const Clip original = *target;
                           std::vector<Clip> rebuilt;
                           for (const Clip& existing : track->clips()) {
                               if (existing.id != originalId) {
                                   rebuilt.push_back(existing);
                                   continue;
                               }
                               rebuilt.push_back(trimmedOut(original, cut));
                               Clip tail = trimmedIn(original, cut);
                               tail.id = tailId;
                               rebuilt.push_back(std::move(tail));
                           }
                           track->setClips(std::move(rebuilt));
                       });
}

Result<CommandPtr> makeLift(Project& project, const EditTarget& target, ClipId clipId) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    if (auto found = requireClip(*located->track, clipId); !found) {
        return found.error();
    }
    const TrackId trackId = target.track;
    return makeCommand(target.sequence, "Lift", {}, [clipId, trackId](Sequence& sequence) {
        Track* track = sequence.findTrack(trackId);
        ZARO_CHECK(track != nullptr, "track vanished between build and apply");
        track->remove(clipId);
    });
}

Result<CommandPtr> makeExtract(Project& project, const EditTarget& target, ClipId clipId) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    auto found = requireClip(*located->track, clipId);
    if (!found) {
        return found.error();
    }
    const TimeRange range = (*found)->timelineRange;
    const TrackId trackId = target.track;

    return makeCommand(target.sequence, "Extract", {},
                       [clipId, range, trackId](Sequence& sequence) {
                           Track* track = sequence.findTrack(trackId);
                           ZARO_CHECK(track != nullptr, "track vanished between build and apply");
                           track->remove(clipId);
                           track->shiftFrom(range.endExclusive(), -range.duration());
                       });
}

Result<CommandPtr> makeRippleDelete(Project& project, const EditTarget& target,
                                    const TimeRange& range, bool rippleAllTracks) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    const time::Rational& rate = located->sequence->frameRate();
    const TimeRange span = range.rescaledTo(rate);
    if (span.isEmpty()) {
        return Error{ErrorCode::InvalidData, "nothing to delete: the range is empty"};
    }

    // No feasibility check is needed here: clearing the range frees exactly the
    // duration that the shift then closes, on every track that is rippled, so
    // the shifted clips can never reach past what was vacated.
    model::IdGenerator& ids = project.ids();
    const TrackId trackId = target.track;

    return makeCommand(target.sequence, "Ripple delete", {},
                       [span, trackId, rippleAllTracks, &ids](Sequence& sequence) {
                           Track* track = sequence.findTrack(trackId);
                           ZARO_CHECK(track != nullptr, "track vanished between build and apply");

                           if (rippleAllTracks) {
                               for (const model::TrackKind kind :
                                    {model::TrackKind::Video, model::TrackKind::Audio}) {
                                   for (Track& other : sequence.tracksMutable(kind)) {
                                       if (!other.isLocked()) {
                                           clearRange(other, span, ids);
                                       }
                                   }
                               }
                           } else {
                               clearRange(*track, span, ids);
                           }
                           rippleFrom(sequence, *track, span.endExclusive(), -span.duration(),
                                      rippleAllTracks);
                       });
}

// --- Trimming ---------------------------------------------------------------

namespace {

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
    return makeCommand(target.sequence, edge == Edge::In ? "Trim in" : "Trim out",
                       "trim:" + idText(clipId) + (edge == Edge::In ? ":in" : ":out"),
                       [result = plan->result, clipId, trackId](Sequence& sequence) {
                           Track* track = sequence.findTrack(trackId);
                           ZARO_CHECK(track != nullptr, "track vanished between build and apply");
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

// --- Structure --------------------------------------------------------------

Result<CommandPtr> makeAddTrack(Project& project, model::SequenceId sequenceId,
                                model::TrackKind kind, std::string name) {
    if (project.findSequence(sequenceId) == nullptr) {
        return Error{ErrorCode::NotFound, "no such sequence"};
    }
    const TrackId id = project.ids().next<model::TrackTag>();
    return makeCommand(sequenceId, "Add " + std::string{model::toString(kind)} + " track", {},
                       [id, kind, name = std::move(name)](Sequence& sequence) {
                           sequence.addTrack(id, kind, name);
                       });
}

Result<CommandPtr> makeRemoveTrack(Project& project, model::SequenceId sequenceId,
                                   TrackId trackId) {
    const Sequence* sequence = project.findSequence(sequenceId);
    if (sequence == nullptr) {
        return Error{ErrorCode::NotFound, "no such sequence"};
    }
    const model::Track* track = sequence->findTrack(trackId);
    if (track == nullptr) {
        return Error{ErrorCode::NotFound, "no such track"};
    }
    if (track->isLocked()) {
        return Error{ErrorCode::Unsupported, "track '" + track->name() + "' is locked"};
    }
    return makeCommand(sequenceId, "Delete track", {},
                       [trackId](Sequence& s) { s.removeTrack(trackId); });
}

}  // namespace zaro::edit
