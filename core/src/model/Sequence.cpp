#include "zaro/core/model/Sequence.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "zaro/core/Check.h"

namespace zaro::model {

void Sequence::setMarkers(std::vector<Marker> markers) {
    // Kept in time order, so drawing and jumping can both walk them in one
    // direction rather than each sorting a copy.
    std::sort(markers.begin(), markers.end(),
              [](const Marker& a, const Marker& b) { return a.range.start() < b.range.start(); });
    markers_ = std::move(markers);
}

const Marker* Sequence::markerAt(const time::RationalTime& t) const {
    for (const Marker& marker : markers_) {
        if (marker.range.contains(t)) {
            return &marker;
        }
        // A point marker has a range of one frame, so containment covers it.
    }
    return nullptr;
}

const Marker* Sequence::markerAfter(const time::RationalTime& t) const {
    for (const Marker& marker : markers_) {
        if (marker.range.start() > t) {
            return &marker;
        }
    }
    return nullptr;
}

const Marker* Sequence::markerBefore(const time::RationalTime& t) const {
    const Marker* best = nullptr;
    for (const Marker& marker : markers_) {
        if (marker.range.start() < t) {
            best = &marker;
        } else {
            break;
        }
    }
    return best;
}

Track* Sequence::findTrack(TrackId id) {
    for (auto* list : {&videoTracks_, &audioTracks_}) {
        for (Track& track : *list) {
            if (track.id() == id) {
                return &track;
            }
        }
    }
    return nullptr;
}

const Track* Sequence::findTrack(TrackId id) const {
    return const_cast<Sequence*>(this)->findTrack(
        id);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
}

TrackId Sequence::addTrack(TrackId id, TrackKind kind, std::string name) {
    ZARO_CHECK(id.isValid(), "adding a track with no id");
    ZARO_CHECK(findTrack(id) == nullptr, "adding a track id that already exists");
    tracksMutable(kind).emplace_back(id, kind, std::move(name));
    return id;
}

void Sequence::removeTrack(TrackId id) {
    for (auto* list : {&videoTracks_, &audioTracks_}) {
        const auto it =
            std::find_if(list->begin(), list->end(), [id](const Track& t) { return t.id() == id; });
        if (it != list->end()) {
            list->erase(it);
            return;
        }
    }
    ZARO_CHECK(false, "removing a track that does not exist");
}

time::RationalTime Sequence::duration() const {
    time::RationalTime longest{0, frameRate_};
    for (const auto* list : {&videoTracks_, &audioTracks_}) {
        for (const Track& track : *list) {
            if (!track.isEmpty()) {
                longest = std::max(longest, track.extent().endExclusive());
            }
        }
    }
    return longest;
}

bool Sequence::hasSolo(TrackKind kind) const {
    // One kind only. Solo answers "of the things I could be shown, show me
    // just this one", and picture and sound are two separate sets of things:
    // a soloed video track says nothing about what should be heard.
    const std::vector<Track>& list = kind == TrackKind::Video ? videoTracks_ : audioTracks_;
    for (const Track& track : list) {
        if (track.isSoloed()) {
            return true;
        }
    }
    return false;
}

bool Sequence::isAudible(const Track& track) const {
    if (track.isMuted()) {
        // Mute wins over solo. Soloing a track someone has muted and hearing it
        // anyway would make mute mean nothing.
        return false;
    }
    return track.isSoloed() || !hasSolo(track.kind());
}

const Clip* findClip(const Sequence& sequence, ClipId id) {
    if (!id.isValid()) {
        return nullptr;
    }
    for (const auto* list : {&sequence.videoTracks(), &sequence.audioTracks()}) {
        for (const Track& track : *list) {
            if (const Clip* found = track.find(id)) {
                return found;
            }
        }
    }
    return nullptr;
}

Transform pinnedTransformAt(const Sequence& sequence, const Clip& clip,
                            const time::RationalTime& at) {
    Transform own = clip.transformAt(at);
    // Bounded rather than recursive without limit: the edit operations refuse
    // to make a cycle, and a file that arrived with one still must not spin.
    constexpr int kMaxChain = 8;
    const Clip* following = &clip;
    for (int step = 0; step < kMaxChain; ++step) {
        const Clip* host = findClip(sequence, following->pinnedTo);
        if (host == nullptr || !host->enabled) {
            break;
        }
        // Only where the host is actually on screen: outside its own range
        // there is no transform to follow, and extrapolating one would put the
        // title somewhere nothing on the timeline explains.
        if (at < host->start() || at >= host->endExclusive()) {
            break;
        }
        const Transform outer = host->transformAt(at);
        const double radians = outer.rotationDegrees * std::numbers::pi / 180.0;
        const double cosine = std::cos(radians);
        const double sine = std::sin(radians);
        const double scaledX = own.positionX * outer.scaleX;
        const double scaledY = own.positionY * outer.scaleY;
        own.positionX = outer.positionX + (scaledX * cosine) - (scaledY * sine);
        own.positionY = outer.positionY + (scaledX * sine) + (scaledY * cosine);
        own.scaleX *= outer.scaleX;
        own.scaleY *= outer.scaleY;
        own.rotationDegrees += outer.rotationDegrees;
        // Opacity is not inherited: see Clip::pinnedTo.
        following = host;
    }
    return own;
}

}  // namespace zaro::model
