#include "zaro/core/edit/Command.h"

#include <cstdint>

#include "zaro/core/Check.h"

namespace zaro::edit {

SequenceCommand::SequenceCommand(model::SequenceId sequence, std::string description,
                                 std::string mergeKey)
    : sequenceId_{sequence}, description_{std::move(description)}, mergeKey_{std::move(mergeKey)} {}

void SequenceCommand::apply(model::Project& project) {
    model::Sequence* sequence = project.findSequence(sequenceId_);
    ZARO_CHECK(sequence != nullptr, "applying a command to a sequence that no longer exists");

    if (!captured_) {
        before_ = *sequence;
        mutate(*sequence);
        after_ = *sequence;
        captured_ = true;
        return;
    }
    // Redo: restore the recorded result rather than replaying the edit, so a
    // command whose effect depends on state it did not capture cannot drift.
    *sequence = after_;
}

void SequenceCommand::revert(model::Project& project) {
    model::Sequence* sequence = project.findSequence(sequenceId_);
    ZARO_CHECK(sequence != nullptr, "reverting a command on a sequence that no longer exists");
    ZARO_CHECK(captured_, "reverting a command that was never applied");
    *sequence = before_;
}

bool SequenceCommand::mergeWith(const Command& newer) {
    const auto* other = dynamic_cast<const SequenceCommand*>(&newer);
    if (other == nullptr || other->sequenceId_ != sequenceId_ || !other->captured_) {
        return false;
    }
    // Keep this command's starting point and adopt the newer one's result: the
    // pair now describes the whole gesture as a single step.
    after_ = other->after_;
    description_ = other->description_;
    return true;
}

std::size_t SequenceCommand::snapshotBytes() const {
    std::size_t bytes = 0;
    for (const model::Sequence* state : {&before_, &after_}) {
        for (const auto* list : {&state->videoTracks(), &state->audioTracks()}) {
            for (const model::Track& track : *list) {
                bytes += track.clips().size() * sizeof(model::Clip);
            }
        }
    }
    return bytes;
}

ProjectCommand::ProjectCommand(std::string description, std::string mergeKey)
    : description_{std::move(description)}, mergeKey_{std::move(mergeKey)} {}

void ProjectCommand::apply(model::Project& project) {
    if (!captured_) {
        before_ = project;
        mutate(project);
        after_ = project;
        captured_ = true;
        return;
    }
    project = after_;
}

void ProjectCommand::revert(model::Project& project) {
    ZARO_CHECK(captured_, "reverting a command that was never applied");
    // The id counter is not part of Project's equality but is part of its
    // state, and rolling it back would let a later import reissue an id that
    // something undone still refers to. Snapshot restore keeps it moving
    // forward, which is what the generator promises.
    const std::uint64_t highestIssued = project.ids().peek();
    project = before_;
    project.ids().observe(highestIssued - 1);
}

bool ProjectCommand::mergeWith(const Command& newer) {
    const auto* other = dynamic_cast<const ProjectCommand*>(&newer);
    if (other == nullptr || !other->captured_) {
        return false;
    }
    after_ = other->after_;
    description_ = other->description_;
    return true;
}

}  // namespace zaro::edit
