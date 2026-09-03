// Putting clips on a timeline and taking them off again.
//
// Overwrite, insert, move, razor, lift, extract and the ripple deletes: the
// operations whose subject is where a clip sits and what happens to its
// neighbours when it moves.

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

/// Take a span out of a caption track, moving what follows earlier.
///
/// A caption wholly inside the span goes; one straddling it keeps the parts
/// outside, joined -- the words either side of a cut are still the words either
/// side of it, and dropping the whole line because one word went would take
/// out speech nobody removed.
void removeSpanFromCaptions(model::CaptionTrack& captions, const TimeRange& span) {
    std::vector<model::Caption> rebuilt;
    const RationalTime length = span.duration();
    for (const model::Caption& caption : captions.captions()) {
        const RationalTime start = caption.range.start();
        const RationalTime end = caption.range.endExclusive();
        if (end <= span.start()) {
            rebuilt.push_back(caption);
            continue;
        }
        if (start >= span.endExclusive()) {
            model::Caption moved = caption;
            moved.range = TimeRange{start - length, caption.range.duration()};
            rebuilt.push_back(std::move(moved));
            continue;
        }
        // Overlapping. What survives is whatever sat outside the span, closed
        // up: a caption that had two seconds before the cut and one after it
        // becomes a three-second caption.
        const RationalTime before =
            start < span.start() ? span.start() - start : RationalTime{0, start.rate()};
        const RationalTime after =
            end > span.endExclusive() ? end - span.endExclusive() : RationalTime{0, start.rate()};
        const RationalTime remaining = before + after;
        if (remaining.frames() <= 0) {
            continue;  // it was entirely inside the span
        }
        model::Caption kept = caption;
        const RationalTime newStart = start < span.start() ? start : span.start();
        kept.range = TimeRange{newStart, remaining};
        rebuilt.push_back(std::move(kept));
    }
    // Rebuilt through add() rather than assigned: the track keeps its captions
    // in order and non-overlapping, and that is its rule to enforce, not this
    // function's to assume.
    captions.clear();
    for (const model::Caption& caption : rebuilt) {
        captions.add(caption);
    }
}

}  // namespace

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

Result<CommandPtr> makePasteClips(Project& project, model::SequenceId sequenceId,
                                  const std::vector<PastedClip>& clips) {
    Sequence* sequence = project.findSequence(sequenceId);
    if (sequence == nullptr) {
        return Error{ErrorCode::NotFound, "no such sequence"};
    }
    if (clips.empty()) {
        return Error{ErrorCode::InvalidData, "there is nothing to paste"};
    }

    // Everything is checked before anything is placed. A paste that put two of
    // four clips down and then found the third had nowhere to go would leave a
    // timeline nobody asked for, and the undo for it would be the caller's
    // problem rather than the command's.
    for (const PastedClip& pasted : clips) {
        const Track* track = sequence->findTrack(pasted.track);
        if (track == nullptr) {
            return Error{ErrorCode::NotFound,
                         "the track one of those clips came from is not in this sequence"};
        }
        if (track->isLocked()) {
            return Error{ErrorCode::Unsupported, "one of those clips would land on a locked track"};
        }
        if (pasted.clip.timelineRange.duration().isZero()) {
            return Error{ErrorCode::InvalidData, "a clip with no duration cannot be pasted"};
        }
        if (pasted.clip.start().frames() < 0) {
            return Error{ErrorCode::InvalidData, "that would put a clip before the start"};
        }
        if (!pasted.clip.id.isValid()) {
            return Error{ErrorCode::InvalidData, "pasted clips need ids; see the header"};
        }
        if (track->find(pasted.clip.id) != nullptr) {
            return Error{ErrorCode::InvalidData, "that clip id is already on the timeline"};
        }
        if (Status fits = checkSourceFits(project, pasted.clip); !fits) {
            return fits.error();
        }
    }

    model::IdGenerator& ids = project.ids();
    return makeCommand(sequenceId, clips.size() == 1 ? "Paste clip" : "Paste clips", {},
                       [clips, &ids](Sequence& sequence) {
                           for (const PastedClip& pasted : clips) {
                               Track* track = sequence.findTrack(pasted.track);
                               ZARO_CHECK(track != nullptr,
                                          "track vanished between build and apply");
                               clearRange(*track, pasted.clip.timelineRange, ids);
                               track->insert(pasted.clip);
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

    // Picture and sound are cut together. A link group is edited as one
    // everywhere else -- moved, trimmed, lifted, extracted -- and a razor that
    // cut only the track under the pointer left the other half of the take
    // joined to itself, which is the one edit whose damage does not show until
    // something is moved and the sound stays behind.
    struct Piece {
        TrackId track;
        ClipId original;
        ClipId tail;
    };
    std::vector<Piece> pieces;
    for (const auto& [otherTrack, otherClip] :
         linkedGroup(*located->sequence, target.track, clip->id)) {
        const Track* track = located->sequence->findTrack(otherTrack);
        const Clip* partner = track != nullptr ? track->find(otherClip) : nullptr;
        // Strictly inside, and per clip: a link group's clips need not span
        // identical ranges, and a cut on a boundary would make a clip of no
        // length. A partner the cut misses is simply not cut.
        if (partner == nullptr || !(partner->start() < cut && cut < partner->endExclusive())) {
            continue;
        }
        pieces.push_back({otherTrack, partner->id, project.ids().next<model::ClipTag>()});
    }

    // The pieces on the right of the cut become a link group of their own.
    // Left in the original group, the head of the picture would be linked to
    // the tail of the sound, and dragging one half of the cut would take the
    // other half's audio with it.
    const model::LinkId tailLink =
        pieces.size() > 1 ? project.ids().next<model::LinkTag>() : model::LinkId{};

    return makeCommand(target.sequence, "Razor", {}, [cut, pieces, tailLink](Sequence& sequence) {
        for (const Piece& piece : pieces) {
            Track* track = sequence.findTrack(piece.track);
            if (track == nullptr) {
                continue;
            }
            const Clip* found = track->find(piece.original);
            if (found == nullptr || !(found->start() < cut && cut < found->endExclusive())) {
                continue;
            }
            const Clip original = *found;
            std::vector<Clip> rebuilt;
            for (const Clip& existing : track->clips()) {
                if (existing.id != piece.original) {
                    rebuilt.push_back(existing);
                    continue;
                }
                rebuilt.push_back(trimmedOut(original, cut));
                Clip tail = trimmedIn(original, cut);
                tail.id = piece.tail;
                if (tailLink.isValid()) {
                    tail.link = tailLink;
                }
                rebuilt.push_back(std::move(tail));
            }
            track->setClips(std::move(rebuilt));
        }
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

Result<CommandPtr> makeDeleteSpans(Project& project, model::SequenceId sequenceId,
                                   const std::vector<TimeRange>& spans) {
    const Sequence* sequence = project.findSequence(sequenceId);
    if (sequence == nullptr) {
        return Error{ErrorCode::NotFound, "no such sequence"};
    }
    const time::Rational& rate = sequence->frameRate();

    std::vector<TimeRange> wanted;
    for (const TimeRange& span : spans) {
        const TimeRange at = span.rescaledTo(rate);
        if (!at.isEmpty()) {
            wanted.push_back(at);
        }
    }
    if (wanted.empty()) {
        return Error{ErrorCode::InvalidData, "there is nothing selected to delete"};
    }
    std::sort(wanted.begin(), wanted.end(),
              [](const TimeRange& a, const TimeRange& b) { return a.start() < b.start(); });

    // Merged before anything is removed: two selections that touch are one
    // removal, and removing them separately would take the overlap out twice.
    std::vector<TimeRange> merged;
    for (const TimeRange& at : wanted) {
        if (!merged.empty() && at.start() <= merged.back().endExclusive()) {
            const RationalTime end = std::max(merged.back().endExclusive(), at.endExclusive());
            merged.back() = TimeRange{merged.back().start(), end - merged.back().start()};
            continue;
        }
        merged.push_back(at);
    }

    model::IdGenerator& ids = project.ids();
    return makeCommand(sequenceId, merged.size() == 1 ? "Delete span" : "Delete spans", {},
                       [merged, &ids](Sequence& seq) {
                           // Latest first: closing one gap moves everything after it, so a
                           // span removed by position early would be somewhere else by the
                           // time its turn came.
                           for (auto span = merged.rbegin(); span != merged.rend(); ++span) {
                               Track* first = nullptr;
                               for (const model::TrackKind kind :
                                    {model::TrackKind::Video, model::TrackKind::Audio}) {
                                   for (Track& track : seq.tracksMutable(kind)) {
                                       if (track.isLocked()) {
                                           continue;
                                       }
                                       clearRange(track, *span, ids);
                                       if (first == nullptr) {
                                           first = &track;
                                       }
                                   }
                               }
                               if (first != nullptr) {
                                   rippleFrom(seq, *first, span->endExclusive(), -span->duration(),
                                              true);
                               }
                               removeSpanFromCaptions(seq.captions(), *span);
                           }
                       });
}

}  // namespace zaro::edit
