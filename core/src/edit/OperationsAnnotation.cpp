// Markers, links between clips, and transitions.
//
// Three things that are about the relationship between clips or about what
// is written on a sequence, rather than about any one clip.

#include <algorithm>
#include <cmath>
#include <cstdint>
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
using detail::locate;
using detail::makeCommand;
using detail::requireClip;

}  // namespace

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

Result<CommandPtr> makeSetMarkerReview(Project& project, model::SequenceId sequenceId,
                                       model::MarkerId markerId, std::string author,
                                       bool resolved) {
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

    return makeCommand(sequenceId, resolved ? "Resolve comment" : "Reopen comment",
                       "review:" + std::to_string(markerId.value()),
                       [markerId, author = std::move(author), resolved](Sequence& seq) {
                           std::vector<model::Marker> rebuilt = seq.markers();
                           for (model::Marker& marker : rebuilt) {
                               if (marker.id == markerId) {
                                   marker.author = author;
                                   marker.resolved = resolved;
                               }
                           }
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

Result<CommandPtr> makeSetTransitionKind(Project& project, const EditTarget& target,
                                         model::TransitionId transitionId,
                                         model::TransitionKind kind,
                                         model::TransitionDirection direction) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    bool found = false;
    for (const model::Transition& transition : located->track->transitions()) {
        found = found || transition.id == transitionId;
    }
    if (!found) {
        return Error{ErrorCode::NotFound, "no such transition on that track"};
    }

    const TrackId trackId = target.track;
    return makeCommand(target.sequence, "Change transition",
                       "transition:" + std::to_string(transitionId.value()),
                       [transitionId, kind, direction, trackId](Sequence& sequence) {
                           Track* track = sequence.findTrack(trackId);
                           ZARO_CHECK(track != nullptr, "track vanished between build and apply");
                           auto transitions = track->transitions();
                           for (model::Transition& transition : transitions) {
                               if (transition.id == transitionId) {
                                   transition.kind = kind;
                                   transition.direction = direction;
                               }
                           }
                           track->setTransitions(std::move(transitions));
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

}  // namespace zaro::edit
