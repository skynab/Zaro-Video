#include "zaro/core/edit/CommandStack.h"

#include "zaro/core/Check.h"

namespace zaro::edit {

void CommandStack::execute(model::Project& project, CommandPtr command) {
    ZARO_CHECK(command != nullptr, "executing a null command");

    // Anything that was undone is now unreachable: the timeline has diverged
    // from the branch those commands described.
    if (savedKnown_ && savedPosition_ > position_) {
        // The saved state was on that branch. It cannot be returned to, so it
        // cannot be recognised either.
        savedKnown_ = false;
    }
    commands_.resize(position_);

    command->apply(project);

    // Inside a group, everything folds into the step the group opened --
    // regardless of key, which is the whole point: five clips is five keys and
    // one gesture. Outside one, only commands that name the same thing merge.
    const std::string key = command->mergeKey();
    const bool joinsGroup = groupDepth_ > 0 && groupJoined_;
    const bool joinsByKey = !key.empty() && !mergeBroken_ && !commands_.empty() &&
                            commands_.back()->mergeKey() == key;
    if ((joinsGroup || joinsByKey) && !commands_.empty() && commands_.back()->mergeWith(*command)) {
        // Folded into the previous step, so position_ does not move -- but the
        // project did. If the saved state was this position, it no longer is:
        // without this, dragging a value that merges would leave the project
        // reading as unmodified while it had changed.
        if (savedKnown_ && savedPosition_ == position_) {
            savedKnown_ = false;
        }
        return;
    }

    commands_.push_back(std::move(command));
    position_ = commands_.size();
    mergeBroken_ = false;
    if (groupDepth_ > 0) {
        groupJoined_ = true;
    }

    while (commands_.size() > maxDepth_) {
        commands_.erase(commands_.begin());
        --position_;
        // The state before the dropped command can no longer be undone back to.
        if (savedKnown_) {
            if (savedPosition_ == 0) {
                savedKnown_ = false;
            } else {
                --savedPosition_;
            }
        }
    }
}

void CommandStack::beginGroup() noexcept {
    if (groupDepth_ == 0) {
        // Nothing before the group joins it: a group is one gesture, and the
        // one that came before it was another.
        mergeBroken_ = true;
        groupJoined_ = false;
    }
    ++groupDepth_;
}

void CommandStack::endGroup() noexcept {
    if (groupDepth_ == 0) {
        return;
    }
    --groupDepth_;
    if (groupDepth_ == 0) {
        groupJoined_ = false;
        mergeBroken_ = true;
    }
}

bool CommandStack::undo(model::Project& project) {
    if (!canUndo()) {
        return false;
    }
    --position_;
    commands_[position_]->revert(project);
    mergeBroken_ = true;
    return true;
}

bool CommandStack::redo(model::Project& project) {
    if (!canRedo()) {
        return false;
    }
    commands_[position_]->apply(project);
    ++position_;
    mergeBroken_ = true;
    return true;
}

std::string CommandStack::undoDescription() const {
    return canUndo() ? commands_[position_ - 1]->description() : std::string{};
}

std::string CommandStack::redoDescription() const {
    return canRedo() ? commands_[position_]->description() : std::string{};
}

std::vector<std::string> CommandStack::history() const {
    std::vector<std::string> out;
    out.reserve(commands_.size());
    for (auto it = commands_.rbegin(); it != commands_.rend(); ++it) {
        out.push_back((*it)->description());
    }
    return out;
}

void CommandStack::clear() {
    commands_.clear();
    position_ = 0;
    mergeBroken_ = true;
    // Throwing away the history throws away the ability to recognise the saved
    // state within it. A caller that clears because it just loaded a file marks
    // the result saved itself.
    savedKnown_ = false;
    savedPosition_ = 0;
}

void CommandStack::markSaved() noexcept {
    savedPosition_ = position_;
    savedKnown_ = true;
}

bool CommandStack::isModified() const noexcept {
    return !savedKnown_ || position_ != savedPosition_;
}

std::size_t CommandStack::snapshotBytes() const {
    std::size_t bytes = 0;
    for (const CommandPtr& command : commands_) {
        if (const auto* sequenceCommand = dynamic_cast<const SequenceCommand*>(command.get())) {
            bytes += sequenceCommand->snapshotBytes();
        }
    }
    return bytes;
}

}  // namespace zaro::edit
