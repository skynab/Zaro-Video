#include "zaro/core/model/Sequence.h"

#include <algorithm>

#include "zaro/core/Check.h"

namespace zaro::model {

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

}  // namespace zaro::model
