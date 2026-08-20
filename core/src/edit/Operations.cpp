#include "zaro/core/edit/Operations.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
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
            // The edited track always moves. Everything else moves only if it
            // is sync locked -- that is exactly what the control is for, and it
            // is how a music bed is kept from sliding every time picture is
            // trimmed.
            const bool follows = other.id() == track.id() || other.isSyncLocked();
            if (!other.isLocked() && follows) {
                other.shiftFrom(from, delta);
            }
        }
    }
}

RationalTime atRate(const RationalTime& t, const time::Rational& rate) {
    return t.rescaledTo(rate);
}

/// Every clip sharing a link with `clip`, including it, across all tracks.
///
/// An unlinked clip yields just itself, so operations can treat linked and
/// unlinked the same way and the unlinked case behaves exactly as it did before
/// links existed.
std::vector<std::pair<TrackId, ClipId>> linkedGroup(Sequence& sequence, TrackId trackId,
                                                    ClipId clipId) {
    std::vector<std::pair<TrackId, ClipId>> group;

    const Track* origin = sequence.findTrack(trackId);
    const Clip* clip = origin != nullptr ? origin->find(clipId) : nullptr;
    if (clip == nullptr) {
        return group;
    }
    if (!clip->link.isValid()) {
        group.emplace_back(trackId, clipId);
        return group;
    }

    const model::LinkId link = clip->link;
    for (const model::TrackKind kind : {model::TrackKind::Video, model::TrackKind::Audio}) {
        for (Track& track : sequence.tracksMutable(kind)) {
            // A locked track keeps its clips where they are, even when
            // something they are linked to moves. Refusing the whole edit
            // instead would make one locked track block editing everywhere.
            if (track.isLocked() && track.id() != trackId) {
                continue;
            }
            for (const Clip& candidate : track.clips()) {
                if (candidate.link == link) {
                    group.emplace_back(track.id(), candidate.id);
                }
            }
        }
    }
    return group;
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
                        // Only tracks that will shift get split; splitting one
                        // that then stays put would leave a cut for no reason.
                        const bool follows = other.id() == trackId || other.isSyncLocked();
                        if (!other.isLocked() && follows) {
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

    const RationalTime shift = start - (*found)->start();

    return makeCommand(target.sequence, "Move clip", "move:" + idText(clipId),
                       [moved, clipId, fromTrack, toTrack, shift, &ids](Sequence& sequence) {
                           Track* source = sequence.findTrack(fromTrack);
                           Track* destination = sequence.findTrack(toTrack);
                           ZARO_CHECK(source != nullptr && destination != nullptr,
                                      "track vanished between build and apply");

                           // Anything linked to this clip moves by the same amount and stays
                           // on its own track: sound follows picture rather than joining it.
                           for (const auto& [otherTrackId, otherClipId] :
                                linkedGroup(sequence, fromTrack, clipId)) {
                               if (otherClipId == clipId) {
                                   continue;
                               }
                               Track* other = sequence.findTrack(otherTrackId);
                               if (other == nullptr) {
                                   continue;
                               }
                               const Clip* existing = other->find(otherClipId);
                               if (existing == nullptr) {
                                   continue;
                               }
                               Clip shifted = *existing;
                               shifted.timelineRange =
                                   TimeRange{existing->start() + shift, existing->duration()};
                               if (shifted.start().frames() < 0) {
                                   continue;
                               }
                               other->remove(otherClipId);
                               clearRange(*other, shifted.timelineRange, ids);
                               other->insert(shifted);
                           }

                           source->remove(clipId);
                           clearRange(*destination, moved.timelineRange, ids);
                           destination->insert(moved);
                       });
}

Result<CommandPtr> makePlaceFromSource(Project& project, const EditTarget& target,
                                       model::MediaRefId mediaId,
                                       const time::TimeRange& sourceRange,
                                       const time::RationalTime& timelineStart, PlaceMode mode) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    const model::MediaRef* media = project.findMedia(mediaId);
    if (media == nullptr) {
        return Error{ErrorCode::NotFound, "that media is not in the project"};
    }
    if (sourceRange.isEmpty()) {
        return Error{ErrorCode::InvalidData, "mark an in and an out point first"};
    }

    const time::Rational& rate = located->sequence->frameRate();
    const time::RationalTime start = atRate(timelineStart, rate);
    if (start.frames() < 0) {
        return Error{ErrorCode::InvalidData, "a clip cannot start before the sequence"};
    }

    // The duration follows from the marked source range, converted to the
    // sequence's rate. Twenty-four frames of a 24fps take is one second, which
    // on a 25fps timeline is twenty-five frames -- not twenty-four.
    const time::RationalTime duration =
        time::RationalTime::fromSeconds(sourceRange.duration().toSeconds(), rate);
    if (duration.frames() <= 0) {
        return Error{ErrorCode::InvalidData,
                     "that range is shorter than a frame at the sequence rate"};
    }

    Clip clip;
    clip.id = project.ids().next<model::ClipTag>();
    clip.source = mediaId;
    clip.name = media->name.empty() ? media->path : media->name;
    clip.sourceRange = sourceRange;
    clip.timelineRange = time::TimeRange{start, duration};

    if (Status fits = checkSourceFits(project, clip); !fits) {
        return fits.error();
    }

    return mode == PlaceMode::Insert ? makeInsert(project, target, std::move(clip))
                                     : makeOverwrite(project, target, std::move(clip));
}

// --- Several clips at once --------------------------------------------------

namespace {

/// The selection, plus everything linked to any of it, with duplicates removed.
std::vector<ClipRef> withLinkedPartners(Sequence& sequence, const std::vector<ClipRef>& clips) {
    std::vector<ClipRef> all;
    const auto alreadyThere = [&all](TrackId track, ClipId clip) {
        return std::any_of(all.begin(), all.end(), [&](const ClipRef& ref) {
            return ref.track == track && ref.clip == clip;
        });
    };

    for (const ClipRef& ref : clips) {
        for (const auto& [track, clip] : linkedGroup(sequence, ref.track, ref.clip)) {
            if (!alreadyThere(track, clip)) {
                all.push_back(ClipRef{track, clip});
            }
        }
    }
    return all;
}

}  // namespace

Result<CommandPtr> makeMoveClips(Project& project, model::SequenceId sequenceId,
                                 const std::vector<ClipRef>& clips,
                                 const time::RationalTime& delta) {
    Sequence* sequence = project.findSequence(sequenceId);
    if (sequence == nullptr) {
        return Error{ErrorCode::NotFound, "no such sequence"};
    }
    if (clips.empty()) {
        return Error{ErrorCode::InvalidData, "nothing selected to move"};
    }

    const time::Rational& rate = sequence->frameRate();
    const time::RationalTime shift = atRate(delta, rate);
    if (shift.isZero()) {
        return Error{ErrorCode::InvalidData, "a move of nothing"};
    }

    const std::vector<ClipRef> all = withLinkedPartners(*sequence, clips);
    for (const ClipRef& ref : all) {
        const Track* track = sequence->findTrack(ref.track);
        if (track == nullptr || track->isLocked()) {
            return Error{ErrorCode::Unsupported, "one of those clips is on a locked track"};
        }
        const Clip* clip = track->find(ref.clip);
        if (clip == nullptr) {
            return Error{ErrorCode::NotFound, "one of those clips is not there"};
        }
        if ((clip->start() + shift).frames() < 0) {
            return Error{ErrorCode::InvalidData,
                         "that would move a clip before the start of the sequence"};
        }
    }

    model::IdGenerator& ids = project.ids();
    return makeCommand(sequenceId, all.size() == 1 ? "Move clip" : "Move clips", {},
                       [all, shift, &ids](Sequence& seq) {
                           // Lift everything first, then place it. Moving them one at a time
                           // would have each overwrite the next while the set is mid-flight,
                           // and the result would depend on the order they came in.
                           std::vector<std::pair<TrackId, Clip>> lifted;
                           lifted.reserve(all.size());
                           for (const ClipRef& ref : all) {
                               Track* track = seq.findTrack(ref.track);
                               if (track == nullptr || track->find(ref.clip) == nullptr) {
                                   continue;
                               }
                               Clip clip = track->remove(ref.clip);
                               clip.timelineRange =
                                   TimeRange{clip.start() + shift, clip.duration()};
                               lifted.emplace_back(ref.track, std::move(clip));
                           }
                           for (auto& [trackId, clip] : lifted) {
                               Track* track = seq.findTrack(trackId);
                               if (track == nullptr) {
                                   continue;
                               }
                               clearRange(*track, clip.timelineRange, ids);
                               track->insert(clip);
                           }
                       });
}

Result<CommandPtr> makeRemoveClips(Project& project, model::SequenceId sequenceId,
                                   const std::vector<ClipRef>& clips, bool ripple) {
    Sequence* sequence = project.findSequence(sequenceId);
    if (sequence == nullptr) {
        return Error{ErrorCode::NotFound, "no such sequence"};
    }
    if (clips.empty()) {
        return Error{ErrorCode::InvalidData, "nothing selected to remove"};
    }

    const std::vector<ClipRef> all = withLinkedPartners(*sequence, clips);
    for (const ClipRef& ref : all) {
        const Track* track = sequence->findTrack(ref.track);
        if (track == nullptr || track->find(ref.clip) == nullptr) {
            return Error{ErrorCode::NotFound, "one of those clips is not there"};
        }
        if (track->isLocked()) {
            return Error{ErrorCode::Unsupported, "one of those clips is on a locked track"};
        }
    }

    return makeCommand(
        sequenceId, ripple ? "Extract clips" : "Lift clips", {}, [all, ripple](Sequence& seq) {
            // Latest first, so closing one gap cannot move a clip that is still
            // to be removed out from under its recorded position.
            std::vector<std::pair<TrackId, TimeRange>> removed;
            for (const ClipRef& ref : all) {
                Track* track = seq.findTrack(ref.track);
                if (track == nullptr) {
                    continue;
                }
                const Clip* clip = track->find(ref.clip);
                if (clip == nullptr) {
                    continue;
                }
                removed.emplace_back(ref.track, clip->timelineRange);
                track->remove(ref.clip);
            }
            if (!ripple) {
                return;
            }
            std::sort(removed.begin(), removed.end(), [](const auto& a, const auto& b) {
                return a.second.start() > b.second.start();
            });
            for (const auto& [trackId, range] : removed) {
                Track* track = seq.findTrack(trackId);
                if (track != nullptr &&
                    track->canShiftFrom(range.endExclusive(), -range.duration())) {
                    track->shiftFrom(range.endExclusive(), -range.duration());
                }
            }
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
        // The whole link group, so lifting picture does not leave its sound
        // stranded on the timeline.
        for (const auto& [otherTrack, otherClip] : linkedGroup(sequence, trackId, clipId)) {
            if (Track* track = sequence.findTrack(otherTrack)) {
                if (track->find(otherClip) != nullptr) {
                    track->remove(otherClip);
                }
            }
        }
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
    // Each clip's own range is read inside the command, since a link group's
    // clips need not span identical ranges.
    const TrackId trackId = target.track;

    return makeCommand(target.sequence, "Extract", {}, [clipId, trackId](Sequence& sequence) {
        for (const auto& [otherTrack, otherClip] : linkedGroup(sequence, trackId, clipId)) {
            Track* track = sequence.findTrack(otherTrack);
            if (track == nullptr || track->find(otherClip) == nullptr) {
                continue;
            }
            const TimeRange gap = track->find(otherClip)->timelineRange;
            track->remove(otherClip);
            // Each track closes its own gap, which is the right thing when
            // the linked clips do not span identical ranges.
            if (track->canShiftFrom(gap.endExclusive(), -gap.duration())) {
                track->shiftFrom(gap.endExclusive(), -gap.duration());
            }
        }
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

    return makeCommand(
        target.sequence, "Ripple delete", {},
        [span, trackId, rippleAllTracks, &ids](Sequence& sequence) {
            Track* track = sequence.findTrack(trackId);
            ZARO_CHECK(track != nullptr, "track vanished between build and apply");

            if (rippleAllTracks) {
                for (const model::TrackKind kind :
                     {model::TrackKind::Video, model::TrackKind::Audio}) {
                    for (Track& other : sequence.tracksMutable(kind)) {
                        // Sync lock decides whether a track
                        // takes part at all. Clearing one that
                        // then does not shift would leave it
                        // half-edited -- material gone and the
                        // gap left open -- which is worse than
                        // doing all of it or none.
                        const bool follows = other.id() == trackId || other.isSyncLocked();
                        if (!other.isLocked() && follows) {
                            clearRange(other, span, ids);
                        }
                    }
                }
            } else {
                clearRange(*track, span, ids);
            }
            rippleFrom(sequence, *track, span.endExclusive(), -span.duration(), rippleAllTracks);
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

// --- Clip properties --------------------------------------------------------

namespace {

/// Shared shape of every property change: find the clip, refuse if the track is
/// locked, and rewrite one field in place. None of these can move a clip, so
/// none of them need to check for collisions.
Result<CommandPtr> modifyClip(Project& project, const EditTarget& target, ClipId clipId,
                              std::string description, std::string mergeKey,
                              std::function<void(Clip&)> change) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    if (auto found = requireClip(*located->track, clipId); !found) {
        return found.error();
    }

    const TrackId trackId = target.track;
    return makeCommand(target.sequence, std::move(description), std::move(mergeKey),
                       [clipId, trackId, change = std::move(change)](Sequence& sequence) {
                           Track* track = sequence.findTrack(trackId);
                           ZARO_CHECK(track != nullptr, "track vanished between build and apply");
                           Clip* clip = track->find(clipId);
                           ZARO_CHECK(clip != nullptr, "clip vanished between build and apply");
                           change(*clip);
                       });
}

}  // namespace

Result<CommandPtr> makeSetTransform(Project& project, const EditTarget& target, ClipId clipId,
                                    const model::Transform& transform) {
    return modifyClip(project, target, clipId, "Adjust motion", "transform:" + idText(clipId),
                      [transform](Clip& clip) { clip.transform = transform; });
}

namespace {

/// A merge key naming one keyframe.
///
/// The time is part of it: dragging a value at one moment should coalesce into
/// a single undo step, but setting a value at a second moment is a new
/// keyframe and a separate decision. A key that stopped at the parameter would
/// swallow the first keyframe into the second.
/// The clip, or why not. Refuses a locked track, the same as every other
/// operation: a keyframe is an edit.
Result<const Clip*> lookupClip(Project& project, const EditTarget& target, ClipId id) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    return requireClip(*located->track, id);
}

std::string keyframeKey(ClipId clip, model::Param param, const time::RationalTime& at) {
    return "keyframe:" + idText(clip) + ":" + model::toString(param) + ":" +
           std::to_string(at.frames()) + "@" + at.rate().toString();
}

}  // namespace

Result<CommandPtr> makeSetKeyframe(Project& project, const EditTarget& target, ClipId clipId,
                                   model::Param param, const time::RationalTime& sourceTime,
                                   double value, model::Interpolation interpolation) {
    if (!std::isfinite(value)) {
        return Error{ErrorCode::InvalidData, "a keyframe value has to be a real number"};
    }
    return modifyClip(
        project, target, clipId, std::string{"Set "} + model::toString(param) + " keyframe",
        keyframeKey(clipId, param, sourceTime),
        [param, sourceTime, value, interpolation](Clip& clip) {
            model::Keyframe key;
            key.time = sourceTime;
            key.value = value;
            key.interpolation = interpolation;
            // Replacing an existing keyframe keeps its handles:
            // dragging a value should not silently flatten a
            // curve the user shaped.
            if (const model::Keyframe* existing = clip.animation.curve(param).at(sourceTime)) {
                key.interpolation = existing->interpolation;
                key.out = existing->out;
                key.in = existing->in;
            }
            clip.animation.curve(param).set(key);
        });
}

Result<CommandPtr> makeRemoveKeyframe(Project& project, const EditTarget& target, ClipId clipId,
                                      model::Param param, const time::RationalTime& sourceTime) {
    auto found = lookupClip(project, target, clipId);
    if (!found) {
        return found.error();
    }
    const Clip* existing = *found;
    const model::Curve* curve = existing->animation.find(param);
    if (curve == nullptr || curve->at(sourceTime) == nullptr) {
        return Error{ErrorCode::NotFound, "there is no keyframe there"};
    }
    return modifyClip(project, target, clipId,
                      std::string{"Delete "} + model::toString(param) + " keyframe",
                      // No merge key: deleting two keyframes is two decisions.
                      {}, [param, sourceTime](Clip& clip) {
                          clip.animation.curve(param).removeAt(sourceTime);
                          // A parameter with no keyframes left is not animated,
                          // and an empty curve left behind would say it still
                          // is. The static value takes over, which is the value
                          // the last keyframe was holding everywhere.
                          clip.animation.pruneEmpty();
                      });
}

Result<CommandPtr> makeMoveKeyframe(Project& project, const EditTarget& target, ClipId clipId,
                                    model::Param param, const time::RationalTime& from,
                                    const time::RationalTime& to) {
    auto found = lookupClip(project, target, clipId);
    if (!found) {
        return found.error();
    }
    const Clip* existing = *found;
    const model::Curve* curve = existing->animation.find(param);
    if (curve == nullptr || curve->at(from) == nullptr) {
        return Error{ErrorCode::NotFound, "there is no keyframe there"};
    }
    // Landing on another keyframe would silently destroy it. Refusing leaves
    // the dragged keyframe where it was, which is recoverable; overwriting is
    // not obviously undoable to someone who did not see what was underneath.
    if (from != to && curve->at(to) != nullptr) {
        return Error{ErrorCode::InvalidData, "another keyframe is already there"};
    }
    return modifyClip(project, target, clipId,
                      std::string{"Move "} + model::toString(param) + " keyframe",
                      keyframeKey(clipId, param, from), [param, from, to](Clip& clip) {
                          model::Curve& curve = clip.animation.curve(param);
                          const model::Keyframe* found = curve.at(from);
                          if (found == nullptr) {
                              return;
                          }
                          model::Keyframe moved = *found;
                          moved.time = to;
                          curve.removeAt(from);
                          curve.set(moved);
                      });
}

Result<CommandPtr> makeSetKeyframeInterpolation(Project& project, const EditTarget& target,
                                                ClipId clipId, model::Param param,
                                                const time::RationalTime& sourceTime,
                                                model::Interpolation interpolation) {
    auto found = lookupClip(project, target, clipId);
    if (!found) {
        return found.error();
    }
    const Clip* existing = *found;
    const model::Curve* curve = existing->animation.find(param);
    if (curve == nullptr || curve->at(sourceTime) == nullptr) {
        return Error{ErrorCode::NotFound, "there is no keyframe there"};
    }
    return modifyClip(project, target, clipId,
                      std::string{"Set keyframe to "} + model::toString(interpolation), {},
                      [param, sourceTime, interpolation](Clip& clip) {
                          model::Curve& curve = clip.animation.curve(param);
                          const model::Keyframe* found = curve.at(sourceTime);
                          if (found == nullptr) {
                              return;
                          }
                          model::Keyframe changed = *found;
                          changed.interpolation = interpolation;
                          curve.set(changed);
                      });
}

Result<CommandPtr> makeMoveKeyframesAt(Project& project, const EditTarget& target, ClipId clipId,
                                       const RationalTime& from, const RationalTime& to) {
    auto found = lookupClip(project, target, clipId);
    if (!found) {
        return found.error();
    }
    const Clip* existing = *found;

    bool any = false;
    for (const auto& [param, curve] : existing->animation) {
        if (curve.at(from) == nullptr) {
            continue;
        }
        any = true;
        // Refused wholesale rather than per parameter: moving some of a set of
        // keyframes and silently leaving the rest is worse than moving none.
        if (from != to && curve.at(to) != nullptr) {
            return Error{ErrorCode::InvalidData, "another keyframe is already there"};
        }
    }
    if (!any) {
        return Error{ErrorCode::NotFound, "there are no keyframes there"};
    }

    return modifyClip(project, target, clipId, "Move keyframes",
                      "keyframes:" + idText(clipId) + ":" + std::to_string(from.frames()),
                      [from, to](Clip& clip) {
                          for (model::Param param : model::allParams()) {
                              model::Curve* curve = clip.animation.find(param) != nullptr
                                                        ? &clip.animation.curve(param)
                                                        : nullptr;
                              if (curve == nullptr) {
                                  continue;
                              }
                              const model::Keyframe* at = curve->at(from);
                              if (at == nullptr) {
                                  continue;
                              }
                              model::Keyframe moved = *at;
                              moved.time = to;
                              curve->removeAt(from);
                              curve->set(moved);
                          }
                      });
}

Result<CommandPtr> makeRemoveKeyframesAt(Project& project, const EditTarget& target, ClipId clipId,
                                         const RationalTime& sourceTime) {
    auto found = lookupClip(project, target, clipId);
    if (!found) {
        return found.error();
    }
    bool any = false;
    for (const auto& [param, curve] : (*found)->animation) {
        any = any || curve.at(sourceTime) != nullptr;
    }
    if (!any) {
        return Error{ErrorCode::NotFound, "there are no keyframes there"};
    }

    return modifyClip(project, target, clipId, "Delete keyframes", {}, [sourceTime](Clip& clip) {
        for (model::Param param : model::allParams()) {
            if (clip.animation.find(param) != nullptr) {
                clip.animation.curve(param).removeAt(sourceTime);
            }
        }
        clip.animation.pruneEmpty();
    });
}

Result<CommandPtr> makeSetParameterAnimated(Project& project, const EditTarget& target,
                                            ClipId clipId, model::Param param, bool animated,
                                            const time::RationalTime& timelineTime) {
    auto found = lookupClip(project, target, clipId);
    if (!found) {
        return found.error();
    }
    const Clip* existing = *found;
    const model::Curve* curve = existing->animation.find(param);
    const bool alreadyAnimated = curve != nullptr && !curve->empty();
    if (alreadyAnimated == animated) {
        return Error{ErrorCode::InvalidData, "that parameter is already in that state"};
    }

    if (animated) {
        // The value it already had, at the moment the stopwatch was pressed, so
        // switching animation on never moves anything.
        const double held = existing->parameterValue(param);
        const time::RationalTime sourceTime = existing->sourceTimeAt(timelineTime);
        return modifyClip(project, target, clipId, std::string{"Animate "} + model::toString(param),
                          {}, [param, sourceTime, held](Clip& clip) {
                              model::Keyframe key;
                              key.time = sourceTime;
                              key.value = held;
                              clip.animation.curve(param).set(key);
                          });
    }

    // Keep what is on screen now. Reverting to the static value underneath
    // would make the picture jump at the instant animation was switched off,
    // and that value is usually the default rather than anything the user
    // chose.
    const double showing = existing->parameterAt(param, timelineTime);
    return modifyClip(project, target, clipId,
                      std::string{"Stop animating "} + model::toString(param), {},
                      [param, showing](Clip& clip) {
                          clip.animation.erase(param);
                          clip.setParameterValue(param, showing);
                      });
}

Result<CommandPtr> makeSetBlendMode(Project& project, const EditTarget& target, ClipId clipId,
                                    model::BlendMode blend) {
    return modifyClip(project, target, clipId,
                      std::string{"Set blend mode to "} + model::toString(blend),
                      "blend:" + idText(clipId), [blend](Clip& clip) { clip.blend = blend; });
}

Result<CommandPtr> makeSetCaptions(Project& project, model::SequenceId sequenceId,
                                   const model::CaptionTrack& captions) {
    if (project.findSequence(sequenceId) == nullptr) {
        return Error{ErrorCode::NotFound, "no such sequence"};
    }
    return makeCommand(sequenceId, "Set captions", {},
                       [captions](Sequence& sequence) { sequence.captions() = captions; });
}

Result<CommandPtr> makeAddGraphic(Project& project, const EditTarget& target,
                                  const model::Graphic& graphic, const time::TimeRange& range) {
    if (range.isEmpty()) {
        return Error{ErrorCode::InvalidData, "a graphic needs a duration"};
    }
    if (!graphic.isSet()) {
        return Error{ErrorCode::InvalidData, "that is not a graphic"};
    }
    Clip clip;
    clip.id = project.ids().next<model::ClipTag>();
    clip.graphic = graphic;
    clip.name = model::toString(graphic.kind);
    clip.timelineRange = range;
    // Its own length, so a trim has something to trim against.
    clip.sourceRange =
        time::TimeRange{time::RationalTime{0, range.start().rate()}, range.duration()};
    return makeOverwrite(project, target, clip);
}

Result<CommandPtr> makeSetMask(Project& project, const EditTarget& target, ClipId clipId,
                               const model::Mask& mask) {
    for (const double value :
         {mask.width, mask.height, mask.centreX, mask.centreY, mask.cornerRadius, mask.feather}) {
        if (!std::isfinite(value)) {
            return Error{ErrorCode::InvalidData, "a mask has to be real numbers"};
        }
    }
    return modifyClip(project, target, clipId, "Adjust mask", "mask:" + idText(clipId),
                      [mask](Clip& clip) { clip.mask = mask; });
}

Result<CommandPtr> makeSetGraphic(Project& project, const EditTarget& target, ClipId clipId,
                                  const model::Graphic& graphic) {
    for (const double value :
         {graphic.width, graphic.height, graphic.centreX, graphic.centreY, graphic.cornerRadius,
          graphic.feather, graphic.red, graphic.green, graphic.blue, graphic.alpha}) {
        if (!std::isfinite(value)) {
            return Error{ErrorCode::InvalidData, "a graphic has to be real numbers"};
        }
    }
    return modifyClip(project, target, clipId, "Adjust graphic", "graphic:" + idText(clipId),
                      [graphic](Clip& clip) { clip.graphic = graphic; });
}

Result<CommandPtr> makeSetLut(Project& project, const EditTarget& target, ClipId clipId,
                              const model::LutRef& lut) {
    if (!std::isfinite(lut.amount)) {
        return Error{ErrorCode::InvalidData, "the LUT amount has to be a real number"};
    }
    return modifyClip(project, target, clipId, lut.path.empty() ? "Clear LUT" : "Set LUT",
                      "lut:" + idText(clipId), [lut](Clip& clip) { clip.lut = lut; });
}

Result<CommandPtr> makeSetSecondary(Project& project, const EditTarget& target, ClipId clipId,
                                    const model::Secondary& secondary) {
    const model::HslQualifier& window = secondary.qualifier;
    for (const double value :
         {window.hueCentre, window.hueWidth, window.hueSoftness, window.saturationLow,
          window.saturationHigh, window.saturationSoftness, window.lumaLow, window.lumaHigh,
          window.lumaSoftness, secondary.correction.temperature, secondary.correction.tint,
          secondary.correction.exposure, secondary.correction.contrast,
          secondary.correction.saturation}) {
        if (!std::isfinite(value)) {
            return Error{ErrorCode::InvalidData, "a secondary has to be real numbers"};
        }
    }
    return modifyClip(project, target, clipId, "Adjust secondary", "secondary:" + idText(clipId),
                      [secondary](Clip& clip) { clip.secondary = secondary; });
}

Result<CommandPtr> makeSetToneCurves(Project& project, const EditTarget& target, ClipId clipId,
                                     const model::ToneCurves& curves) {
    for (const model::ToneCurve* curve :
         {&curves.master, &curves.red, &curves.green, &curves.blue}) {
        for (const model::CurvePoint& point : curve->points()) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
                return Error{ErrorCode::InvalidData, "a curve point has to be real numbers"};
            }
        }
    }
    // One merge key for all four curves: dragging a point is one gesture even
    // though it rewrites the whole set.
    return modifyClip(project, target, clipId, "Adjust curves", "curves:" + idText(clipId),
                      [curves](Clip& clip) { clip.curves = curves; });
}

Result<CommandPtr> makeSetColorCorrection(Project& project, const EditTarget& target, ClipId clipId,
                                          const model::ColorCorrection& color) {
    for (const double value :
         {color.temperature, color.tint, color.exposure, color.contrast, color.saturation}) {
        if (!std::isfinite(value)) {
            return Error{ErrorCode::InvalidData, "a colour correction has to be real numbers"};
        }
    }
    return modifyClip(project, target, clipId, "Adjust colour", "color:" + idText(clipId),
                      [color](Clip& clip) { clip.color = color; });
}

Result<CommandPtr> makeSetClipAudio(Project& project, const EditTarget& target, ClipId clipId,
                                    double gainDb, double pan) {
    if (!std::isfinite(gainDb) || !std::isfinite(pan)) {
        return Error{ErrorCode::InvalidData, "gain and pan have to be real numbers"};
    }
    const double clampedPan = std::clamp(pan, -1.0, 1.0);
    return modifyClip(project, target, clipId, "Adjust audio", "audio:" + idText(clipId),
                      [gainDb, clampedPan](Clip& clip) {
                          clip.gainDb = gainDb;
                          clip.pan = clampedPan;
                      });
}

Result<CommandPtr> makeSetClipEnabled(Project& project, const EditTarget& target, ClipId clipId,
                                      bool enabled) {
    return modifyClip(project, target, clipId, enabled ? "Enable clip" : "Disable clip",
                      // No merge key: this is a toggle, and two of them in a row
                      // are two decisions rather than one gesture.
                      {}, [enabled](Clip& clip) { clip.enabled = enabled; });
}

// --- Markers ----------------------------------------------------------------

Result<CommandPtr> makeAddMarker(Project& project, model::SequenceId sequenceId,
                                 const time::RationalTime& at, const time::RationalTime& duration,
                                 std::string name, std::int32_t colour) {
    Sequence* sequence = project.findSequence(sequenceId);
    if (sequence == nullptr) {
        return Error{ErrorCode::NotFound, "no such sequence"};
    }
    const time::Rational& rate = sequence->frameRate();
    const time::RationalTime start = atRate(at, rate);
    if (start.frames() < 0) {
        return Error{ErrorCode::InvalidData, "a marker cannot sit before the sequence"};
    }

    // Zero becomes one frame. A point marker is a span of one frame, and having
    // a genuinely empty range would make containment tests answer no everywhere,
    // including at the marker itself.
    time::RationalTime span = atRate(duration, rate);
    if (span.frames() < 1) {
        span = time::RationalTime{1, rate};
    }

    model::Marker marker;
    marker.id = project.ids().next<model::MarkerTag>();
    marker.range = time::TimeRange{start, span};
    marker.name = std::move(name);
    marker.colour = colour;

    return makeCommand(sequenceId, "Add marker", {}, [marker](Sequence& seq) {
        std::vector<model::Marker> rebuilt = seq.markers();
        rebuilt.push_back(marker);
        seq.setMarkers(std::move(rebuilt));
    });
}

Result<CommandPtr> makeRemoveMarker(Project& project, model::SequenceId sequenceId,
                                    model::MarkerId markerId) {
    const Sequence* sequence = project.findSequence(sequenceId);
    if (sequence == nullptr) {
        return Error{ErrorCode::NotFound, "no such sequence"};
    }
    const bool exists =
        std::any_of(sequence->markers().begin(), sequence->markers().end(),
                    [markerId](const model::Marker& marker) { return marker.id == markerId; });
    if (!exists) {
        return Error{ErrorCode::NotFound, "no such marker"};
    }

    return makeCommand(sequenceId, "Remove marker", {}, [markerId](Sequence& seq) {
        std::vector<model::Marker> rebuilt = seq.markers();
        std::erase_if(rebuilt,
                      [markerId](const model::Marker& marker) { return marker.id == markerId; });
        seq.setMarkers(std::move(rebuilt));
    });
}

Result<CommandPtr> makeUpdateMarker(Project& project, model::SequenceId sequenceId,
                                    model::MarkerId markerId, std::string name, std::string note,
                                    std::int32_t colour) {
    const Sequence* sequence = project.findSequence(sequenceId);
    if (sequence == nullptr) {
        return Error{ErrorCode::NotFound, "no such sequence"};
    }
    const bool exists =
        std::any_of(sequence->markers().begin(), sequence->markers().end(),
                    [markerId](const model::Marker& marker) { return marker.id == markerId; });
    if (!exists) {
        return Error{ErrorCode::NotFound, "no such marker"};
    }

    return makeCommand(
        sequenceId, "Edit marker", "marker:" + std::to_string(markerId.value()),
        [markerId, name = std::move(name), note = std::move(note), colour](Sequence& seq) {
            std::vector<model::Marker> rebuilt = seq.markers();
            for (model::Marker& marker : rebuilt) {
                if (marker.id == markerId) {
                    marker.name = name;
                    marker.note = note;
                    marker.colour = colour;
                }
            }
            seq.setMarkers(std::move(rebuilt));
        });
}

// --- Linking ----------------------------------------------------------------

Result<CommandPtr> makeLinkClips(Project& project, model::SequenceId sequenceId,
                                 const std::vector<ClipRef>& clips) {
    Sequence* sequence = project.findSequence(sequenceId);
    if (sequence == nullptr) {
        return Error{ErrorCode::NotFound, "no such sequence"};
    }
    if (clips.size() < 2) {
        return Error{ErrorCode::InvalidData, "linking needs at least two clips"};
    }
    for (const ClipRef& ref : clips) {
        const Track* track = sequence->findTrack(ref.track);
        if (track == nullptr || track->find(ref.clip) == nullptr) {
            return Error{ErrorCode::NotFound, "one of those clips is not there"};
        }
    }

    const model::LinkId link = project.ids().next<model::LinkTag>();
    return makeCommand(sequenceId, "Link clips", {}, [clips, link](Sequence& seq) {
        for (const ClipRef& ref : clips) {
            Track* track = seq.findTrack(ref.track);
            if (track == nullptr) {
                continue;
            }
            if (Clip* clip = track->find(ref.clip)) {
                clip->link = link;
            }
        }
    });
}

Result<CommandPtr> makeUnlinkClips(Project& project, const EditTarget& target, ClipId clipId) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    auto found = requireClip(*located->track, clipId);
    if (!found) {
        return found.error();
    }
    if (!(*found)->link.isValid()) {
        return Error{ErrorCode::InvalidData, "that clip is not linked to anything"};
    }

    const model::LinkId link = (*found)->link;
    return makeCommand(target.sequence, "Unlink clips", {}, [link](Sequence& seq) {
        // Clears the whole group rather than just the clip asked about:
        // unlinking one half of a pair and leaving the other pointing at a
        // group of one would be a state nothing else expects.
        for (const model::TrackKind kind : {model::TrackKind::Video, model::TrackKind::Audio}) {
            for (Track& track : seq.tracksMutable(kind)) {
                std::vector<Clip> rebuilt = track.clips();
                for (Clip& clip : rebuilt) {
                    if (clip.link == link) {
                        clip.link = {};
                    }
                }
                track.setClips(std::move(rebuilt));
            }
        }
    });
}

// --- Transitions ------------------------------------------------------------

Result<CommandPtr> makeAddCrossDissolve(Project& project, const EditTarget& target,
                                        const time::RationalTime& at,
                                        const time::RationalTime& duration) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    const Sequence& sequence = *located->sequence;
    const Track& track = *located->track;

    const time::RationalTime when = atRate(at, sequence.frameRate());
    const time::RationalTime span = atRate(duration, sequence.frameRate());
    if (span.frames() <= 0) {
        return Error{ErrorCode::InvalidData, "a transition needs a positive duration"};
    }

    // Find the cut nearest the requested point: the boundary between two clips
    // that meet. A dissolve needs something on both sides of it.
    const std::vector<Clip>& clips = track.clips();
    const Clip* outgoing = nullptr;
    const Clip* incoming = nullptr;
    std::int64_t bestDistance = std::numeric_limits<std::int64_t>::max();

    for (std::size_t i = 0; i + 1 < clips.size(); ++i) {
        if (clips[i].endExclusive() != clips[i + 1].start()) {
            continue;  // a gap, not a cut
        }
        const std::int64_t distance =
            std::abs((clips[i].endExclusive() - when).rescaledTo(sequence.frameRate()).frames());
        if (distance < bestDistance) {
            bestDistance = distance;
            outgoing = &clips[i];
            incoming = &clips[i + 1];
        }
    }
    if (outgoing == nullptr) {
        return Error{ErrorCode::NotFound, "there is no cut here to dissolve across"};
    }

    const time::RationalTime cut = outgoing->endExclusive();
    const time::RationalTime half{span.frames() / 2, sequence.frameRate()};
    const time::RationalTime start = cut - half;
    const time::TimeRange range{start, span};

    if (start.frames() < 0) {
        return Error{ErrorCode::InvalidData, "the dissolve would start before the sequence"};
    }
    // The span cannot reach past either clip: a dissolve longer than the
    // material either side of it has nothing to show at the ends.
    if (start < outgoing->start() || range.endExclusive() > incoming->endExclusive()) {
        return Error{ErrorCode::InvalidData, "the dissolve is longer than the clips it joins"};
    }

    // Both clips are read beyond the cut during the dissolve, so both need
    // handles there.
    Clip extendedOut = *outgoing;
    extendedOut.sourceRange = time::TimeRange::fromStartEnd(
        outgoing->sourceRange.start(), outgoing->sourceTimeAt(range.endExclusive()));
    if (Status fits = checkSourceFits(project, extendedOut); !fits) {
        return Error{fits.error().code(),
                     "the outgoing clip has no handles past the cut: " + fits.error().message()};
    }

    Clip extendedIn = *incoming;
    extendedIn.sourceRange = time::TimeRange::fromStartEnd(incoming->sourceTimeAt(range.start()),
                                                           incoming->sourceRange.endExclusive());
    if (Status fits = checkSourceFits(project, extendedIn); !fits) {
        return Error{fits.error().code(),
                     "the incoming clip has no handles before the cut: " + fits.error().message()};
    }

    model::Transition transition;
    transition.id = project.ids().next<model::TransitionTag>();
    transition.from = outgoing->id;
    transition.to = incoming->id;
    transition.range = range;
    transition.kind = model::TransitionKind::CrossDissolve;

    const TrackId trackId = target.track;
    return makeCommand(target.sequence, "Add cross dissolve", {},
                       [transition, trackId](Sequence& seq) {
                           Track* t = seq.findTrack(trackId);
                           ZARO_CHECK(t != nullptr, "track vanished between build and apply");
                           std::vector<model::Transition> rebuilt = t->transitions();
                           // One transition per cut: adding another across the
                           // same cut replaces it rather than stacking.
                           std::erase_if(rebuilt, [&transition](const model::Transition& other) {
                               return other.from == transition.from && other.to == transition.to;
                           });
                           rebuilt.push_back(transition);
                           t->setTransitions(std::move(rebuilt));
                       });
}

Result<CommandPtr> makeRemoveTransition(Project& project, const EditTarget& target,
                                        model::TransitionId transitionId) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    if (located->track->findTransition(transitionId) == nullptr) {
        return Error{ErrorCode::NotFound, "no such transition on this track"};
    }

    const TrackId trackId = target.track;
    return makeCommand(target.sequence, "Remove transition", {},
                       [transitionId, trackId](Sequence& seq) {
                           Track* t = seq.findTrack(trackId);
                           ZARO_CHECK(t != nullptr, "track vanished between build and apply");
                           std::vector<model::Transition> rebuilt = t->transitions();
                           std::erase_if(rebuilt, [transitionId](const model::Transition& other) {
                               return other.id == transitionId;
                           });
                           t->setTransitions(std::move(rebuilt));
                       });
}

// --- The project ------------------------------------------------------------

namespace {

class ImportMediaCommand final : public ProjectCommand {
public:
    ImportMediaCommand(model::MediaRef media, std::string description)
        : ProjectCommand{std::move(description)}, media_{std::move(media)} {}

protected:
    void mutate(Project& project) override { project.addMedia(media_); }

private:
    model::MediaRef media_;
};

}  // namespace

Result<CommandPtr> makeImportMedia(Project& project, model::MediaRef media) {
    if (media.path.empty()) {
        return Error{ErrorCode::InvalidData, "media needs a path"};
    }
    if (!media.id.isValid()) {
        media.id = project.ids().next<model::MediaRefTag>();
    }
    if (project.findMedia(media.id) != nullptr) {
        return Error{ErrorCode::InvalidData, "that media id is already in the project"};
    }
    const std::string name = media.name.empty() ? media.path : media.name;
    return CommandPtr{std::make_unique<ImportMediaCommand>(std::move(media), "Import " + name)};
}

// --- Structure --------------------------------------------------------------

Result<CommandPtr> makeSetTrackState(Project& project, model::SequenceId sequenceId,
                                     TrackId trackId, const TrackState& state) {
    const Sequence* sequence = project.findSequence(sequenceId);
    if (sequence == nullptr) {
        return Error{ErrorCode::NotFound, "no such sequence"};
    }
    if (sequence->findTrack(trackId) == nullptr) {
        return Error{ErrorCode::NotFound, "no such track in this sequence"};
    }
    if (!std::isfinite(state.gainDb) || !std::isfinite(state.pan)) {
        return Error{ErrorCode::InvalidData, "gain and pan have to be real numbers"};
    }
    const double pan = std::clamp(state.pan, -1.0, 1.0);
    // Keyed by track, so a fader drag is one undo step and the strip next to it
    // is a separate one.
    return makeCommand(sequenceId, "Adjust track", "track:" + std::to_string(trackId.value()),
                       [trackId, state, pan](Sequence& sequence) {
                           Track* track = sequence.findTrack(trackId);
                           if (track == nullptr) {
                               return;
                           }
                           track->setMuted(state.muted);
                           track->setSoloed(state.soloed);
                           track->setGainDb(state.gainDb);
                           track->setPan(pan);
                       });
}

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
