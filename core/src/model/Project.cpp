#include "zaro/core/model/Project.h"

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

}  // namespace zaro::model
