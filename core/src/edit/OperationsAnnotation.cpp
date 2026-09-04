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
    // The nearest edge that is *not* a cut: the head of a run, the tail of one,
    // either side of a gap. A span there is a fade against nothing -- which is
    // what the dissolve button means at the end of a cut, and the thing there
    // was previously no way at all to ask for.
    //
    // Weighed against the nearest cut rather than used only when there is no
    // cut at all: on a track of two clips the tail of the second is a perfectly
    // ordinary place to want a fade out, and the cut behind it is not what
    // somebody pointing at the end of the sequence meant.
    const Clip* fading = nullptr;
    bool atTail = true;
    std::int64_t freeDistance = std::numeric_limits<std::int64_t>::max();
    for (std::size_t i = 0; i < clips.size(); ++i) {
        const bool joinedBefore = i > 0 && clips[i - 1].endExclusive() == clips[i].start();
        const bool joinedAfter =
            i + 1 < clips.size() && clips[i].endExclusive() == clips[i + 1].start();
        for (const bool tail : {false, true}) {
            if (tail ? joinedAfter : joinedBefore) {
                continue;  // that end is a cut, and the search above owns it
            }
            const time::RationalTime edge = tail ? clips[i].endExclusive() : clips[i].start();
            const std::int64_t distance =
                std::abs((edge - when).rescaledTo(sequence.frameRate()).frames());
            if (distance < freeDistance) {
                freeDistance = distance;
                fading = &clips[i];
                atTail = tail;
            }
        }
    }

    // The span lies *inside* the clip rather than straddling a join, so it
    // reads no handles and can always be added -- a fade out on a clip that
    // uses every frame of its source is an ordinary thing to want, and refusing
    // it for want of material past the end would be refusing material the fade
    // never asks for.
    if (fading != nullptr && (outgoing == nullptr || freeDistance < bestDistance)) {
        const Clip* nearest = fading;
        if (span > nearest->duration()) {
            return Error{ErrorCode::InvalidData, "the fade is longer than the clip it is on"};
        }

        model::Transition fade;
        fade.id = project.ids().next<model::TransitionTag>();
        fade.kind = model::TransitionKind::CrossDissolve;
        if (atTail) {
            fade.from = nearest->id;
            fade.range = time::TimeRange{nearest->endExclusive() - span, span};
        } else {
            fade.to = nearest->id;
            fade.range = time::TimeRange{nearest->start(), span};
        }

        const TrackId onTrack = target.track;
        return makeCommand(target.sequence, atTail ? "Add fade out" : "Add fade in", {},
                           [fade, onTrack](Sequence& seq) {
                               Track* t = seq.findTrack(onTrack);
                               ZARO_CHECK(t != nullptr, "track vanished between build and apply");
                               std::vector<model::Transition> rebuilt = t->transitions();
                               // One fade per end, the same rule a cut has:
                               // asking again replaces rather than stacks.
                               std::erase_if(rebuilt, [&fade](const model::Transition& other) {
                                   return other.from == fade.from && other.to == fade.to;
                               });
                               rebuilt.push_back(fade);
                               t->setTransitions(std::move(rebuilt));
                           });
    }

    if (outgoing == nullptr) {
        // Neither a cut nor an edge: an empty track.
        return Error{ErrorCode::NotFound, "there is nothing here to dissolve"};
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

Result<CommandPtr> makeSetTransitionRange(Project& project, const EditTarget& target,
                                          model::TransitionId transitionId,
                                          const time::TimeRange& range) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    const Sequence& sequence = *located->sequence;
    const Track& track = *located->track;
    const model::Transition* existing = track.findTransition(transitionId);
    if (existing == nullptr) {
        return Error{ErrorCode::NotFound, "no such transition on that track"};
    }

    const time::TimeRange wanted{atRate(range.start(), sequence.frameRate()),
                                 atRate(range.duration(), sequence.frameRate())};
    if (wanted.duration().frames() <= 0) {
        return Error{ErrorCode::InvalidData, "a transition needs a positive duration"};
    }
    if (wanted.start().frames() < 0) {
        return Error{ErrorCode::InvalidData, "the transition would start before the sequence"};
    }

    const Clip* outgoing = track.find(existing->from);
    const Clip* incoming = track.find(existing->to);

    // A fade against nothing lies inside its one clip, so the rules are the
    // clip's own edges: it is anchored to the end it fades at and free at the
    // other, which is exactly the edge somebody drags to say how long it is.
    if (existing->isFadeOut() || existing->isFadeIn()) {
        const Clip* on = existing->isFadeOut() ? outgoing : incoming;
        if (on == nullptr) {
            return Error{ErrorCode::NotFound, "the transition's clip is not there"};
        }
        const bool tail = existing->isFadeOut();
        // The anchored end must not move. Dragging the far edge is what sets
        // the length; dragging the fade off its own clip is not a length.
        if (tail ? wanted.endExclusive() != on->endExclusive() : wanted.start() != on->start()) {
            return Error{ErrorCode::InvalidData, "a fade stays at the end it fades at"};
        }
        if (wanted.start() < on->start() || wanted.endExclusive() > on->endExclusive()) {
            return Error{ErrorCode::InvalidData, "the fade is longer than the clip it is on"};
        }

        const TrackId fadeTrack = target.track;
        return makeCommand(target.sequence, "Resize fade",
                           "transition-range:" + std::to_string(transitionId.value()),
                           [transitionId, wanted, fadeTrack](Sequence& seq) {
                               Track* t = seq.findTrack(fadeTrack);
                               ZARO_CHECK(t != nullptr, "track vanished between build and apply");
                               std::vector<model::Transition> rebuilt = t->transitions();
                               for (model::Transition& transition : rebuilt) {
                                   if (transition.id == transitionId) {
                                       transition.range = wanted;
                                   }
                               }
                               t->setTransitions(std::move(rebuilt));
                           });
    }

    if (outgoing == nullptr || incoming == nullptr) {
        return Error{ErrorCode::NotFound, "the transition's clips are not both there"};
    }

    // Still across the cut it was made for. A span dragged clear of the join
    // is not a shorter dissolve, it is a dissolve somewhere there is nothing
    // to dissolve -- and the render reads both clips throughout the span, so
    // it would show the outgoing clip's handles over the incoming one's.
    const time::RationalTime cut = outgoing->endExclusive();
    if (wanted.start() > cut || wanted.endExclusive() < cut) {
        return Error{ErrorCode::InvalidData, "the transition has to stay across its cut"};
    }
    // And no longer than the material either side of it, which is the same
    // rule adding one obeys.
    if (wanted.start() < outgoing->start() || wanted.endExclusive() > incoming->endExclusive()) {
        return Error{ErrorCode::InvalidData, "the transition is longer than the clips it joins"};
    }

    Clip extendedOut = *outgoing;
    extendedOut.sourceRange = time::TimeRange::fromStartEnd(
        outgoing->sourceRange.start(), outgoing->sourceTimeAt(wanted.endExclusive()));
    if (Status fits = checkSourceFits(project, extendedOut); !fits) {
        return Error{fits.error().code(),
                     "the outgoing clip has no handles past the cut: " + fits.error().message()};
    }

    Clip extendedIn = *incoming;
    extendedIn.sourceRange = time::TimeRange::fromStartEnd(incoming->sourceTimeAt(wanted.start()),
                                                           incoming->sourceRange.endExclusive());
    if (Status fits = checkSourceFits(project, extendedIn); !fits) {
        return Error{fits.error().code(),
                     "the incoming clip has no handles before the cut: " + fits.error().message()};
    }

    const TrackId trackId = target.track;
    // Merged under one key, so dragging an edge across twenty frames is one
    // undo step rather than twenty -- the same treatment a trim gets.
    return makeCommand(target.sequence, "Resize transition",
                       "transition-range:" + std::to_string(transitionId.value()),
                       [transitionId, wanted, trackId](Sequence& seq) {
                           Track* t = seq.findTrack(trackId);
                           ZARO_CHECK(t != nullptr, "track vanished between build and apply");
                           std::vector<model::Transition> rebuilt = t->transitions();
                           for (model::Transition& transition : rebuilt) {
                               if (transition.id == transitionId) {
                                   transition.range = wanted;
                               }
                           }
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
