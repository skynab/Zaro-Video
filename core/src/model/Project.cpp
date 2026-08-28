#include "zaro/core/model/Project.h"

#include <cstdint>
#include <set>
#include <vector>

#include "zaro/core/Check.h"

namespace zaro::model {

const MediaRef* Project::findMedia(MediaRefId id) const {
    for (const MediaRef& ref : media_) {
        if (ref.id == id) {
            return &ref;
        }
    }
    return nullptr;
}

Sequence* Project::findSequence(SequenceId id) {
    for (Sequence& sequence : sequences_) {
        if (sequence.id() == id) {
            return &sequence;
        }
    }
    return nullptr;
}

const Sequence* Project::findSequence(SequenceId id) const {
    return const_cast<Project*>(this)->findSequence(
        id);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
}

MediaRefId Project::addMedia(MediaRef ref) {
    ZARO_CHECK(ref.id.isValid(), "adding media with no id");
    ZARO_CHECK(findMedia(ref.id) == nullptr, "adding a media id that already exists");
    const MediaRefId id = ref.id;
    media_.push_back(std::move(ref));
    return id;
}

Project newProject(const std::string& sequenceName) {
    Project project;
    Sequence sequence{project.ids().next<SequenceTag>(), sequenceName, time::rates::fps25};
    sequence.setSize(1920, 1080);
    // One of each. A timeline with no tracks has nowhere to drop anything, and
    // the first thing anybody does is drop something.
    sequence.addTrack(project.ids().next<TrackTag>(), TrackKind::Video, "V1");
    sequence.addTrack(project.ids().next<TrackTag>(), TrackKind::Audio, "A1");
    const SequenceId id = project.addSequence(std::move(sequence));
    project.setActiveSequence(id);
    return project;
}

const Subclip* Project::findSubclip(SubclipId id) const {
    for (const Subclip& subclip : subclips_) {
        if (subclip.id == id) {
            return &subclip;
        }
    }
    return nullptr;
}

SubclipId Project::addSubclip(Subclip subclip) {
    ZARO_CHECK(subclip.id.isValid(), "adding a subclip with no id");
    ZARO_CHECK(findSubclip(subclip.id) == nullptr, "adding a subclip id that already exists");
    const SubclipId id = subclip.id;
    subclips_.push_back(std::move(subclip));
    return id;
}

bool Project::removeSubclip(SubclipId id) {
    for (auto it = subclips_.begin(); it != subclips_.end(); ++it) {
        if (it->id == id) {
            subclips_.erase(it);
            return true;
        }
    }
    return false;
}

SequenceId Project::addSequence(Sequence sequence) {
    ZARO_CHECK(sequence.id().isValid(), "adding a sequence with no id");
    ZARO_CHECK(findSequence(sequence.id()) == nullptr, "adding a sequence id that already exists");
    const SequenceId id = sequence.id();
    sequences_.push_back(std::move(sequence));
    if (!activeSequence_.isValid()) {
        activeSequence_ = id;
    }
    return id;
}

bool Project::nestingWouldCycle(SequenceId outer, SequenceId inner) const {
    if (!outer.isValid() || !inner.isValid()) {
        return false;
    }
    if (outer == inner) {
        return true;
    }

    // Walk what `inner` already contains. If `outer` is anywhere in there,
    // putting `inner` inside it closes the loop.
    std::vector<SequenceId> pending{inner};
    std::set<std::uint64_t> seen;
    while (!pending.empty()) {
        const SequenceId current = pending.back();
        pending.pop_back();
        if (!seen.insert(current.value()).second) {
            continue;  // already walked; a diamond is not a cycle
        }
        const Sequence* sequence = findSequence(current);
        if (sequence == nullptr) {
            continue;
        }
        for (const auto* list : {&sequence->videoTracks(), &sequence->audioTracks()}) {
            for (const Track& track : *list) {
                for (const Clip& clip : track.clips()) {
                    if (!clip.nested.isValid()) {
                        continue;
                    }
                    if (clip.nested == outer) {
                        return true;
                    }
                    pending.push_back(clip.nested);
                }
            }
        }
    }
    return false;
}

}  // namespace zaro::model
