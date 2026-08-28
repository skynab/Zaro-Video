#include "OperationsCommon.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "zaro/core/Check.h"

namespace zaro::edit::detail {
namespace {

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

}  // namespace

CommandPtr makeCommand(model::SequenceId sequence, std::string description, std::string mergeKey,
                       LambdaCommand::Body body) {
    return std::make_unique<LambdaCommand>(sequence, std::move(description), std::move(mergeKey),
                                           std::move(body));
}

std::string idText(ClipId id) {
    return std::to_string(id.value());
}

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

}  // namespace zaro::edit::detail
