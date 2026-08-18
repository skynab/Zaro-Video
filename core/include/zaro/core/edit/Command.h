#pragma once

#include <memory>
#include <string>

#include "zaro/core/model/Project.h"

namespace zaro::edit {

/// A single undoable change.
///
/// Commands are the *only* write path into the model. Nothing else may mutate a
/// Project. Undo that is retrofitted always ends up with one operation that
/// escaped it, and in an editor that operation is the one someone loses an
/// afternoon to.
class Command {
public:
    virtual ~Command() = default;

    Command(const Command&) = delete;
    Command& operator=(const Command&) = delete;

    virtual void apply(model::Project& project) = 0;
    virtual void revert(model::Project& project) = 0;

    /// What the History panel shows. Written as a past-tense action.
    [[nodiscard]] virtual std::string description() const = 0;

    /// Commands sharing a non-empty key coalesce into one undo step when
    /// executed consecutively. Dragging a clip emits a command per mouse move;
    /// undo should step over the whole drag, not three hundred pixels of it.
    [[nodiscard]] virtual std::string mergeKey() const { return {}; }

    /// Fold `newer` into this command. Only called when the merge keys match.
    [[nodiscard]] virtual bool mergeWith(const Command& newer) = 0;

protected:
    Command() = default;
};

using CommandPtr = std::unique_ptr<Command>;

/// A command that edits one sequence, undone by restoring a snapshot of it.
///
/// The alternative -- every operation computing its own precise inverse -- is
/// faster and uses less memory, and it is also where undo bugs live. Ripple
/// trim across a sync group has an inverse that is genuinely hard to get right,
/// and a wrong inverse corrupts a project silently, one step at a time, until
/// someone notices their timeline no longer matches what they cut.
///
/// A snapshot is correct by construction. The cost is bounded and small: a Clip
/// is around a hundred bytes and holds no media, so a thousand-clip sequence
/// snapshots to roughly 100 KB. Both the before and after states are kept so
/// redo is exact rather than a re-execution that might not be deterministic.
///
/// If a project ever appears where this is too expensive -- tens of thousands
/// of clips in one sequence -- the fix is to give the few operations that touch
/// many clips a real inverse, not to abandon the approach for the ninety
/// percent that touch two.
class SequenceCommand : public Command {
public:
    SequenceCommand(model::SequenceId sequence, std::string description, std::string mergeKey = {});

    void apply(model::Project& project) final;
    void revert(model::Project& project) final;

    [[nodiscard]] std::string description() const final { return description_; }
    [[nodiscard]] std::string mergeKey() const final { return mergeKey_; }
    [[nodiscard]] bool mergeWith(const Command& newer) final;

    /// Approximate retained size, for budgeting the undo stack.
    [[nodiscard]] std::size_t snapshotBytes() const;

protected:
    /// The edit itself. Runs once, on first apply.
    virtual void mutate(model::Sequence& sequence) = 0;

private:
    model::SequenceId sequenceId_;
    std::string description_;
    std::string mergeKey_;

    model::Sequence before_;
    model::Sequence after_;
    bool captured_{false};
};

}  // namespace zaro::edit
