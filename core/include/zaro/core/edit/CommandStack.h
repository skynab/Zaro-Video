#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "zaro/core/edit/Command.h"

namespace zaro::edit {

/// Undo and redo history.
///
/// Depth is bounded rather than unlimited: snapshots are cheap but not free,
/// and an editor left open for a week should not accumulate history until it
/// runs out of memory. The oldest entries are dropped first.
class CommandStack {
public:
    static constexpr std::size_t kDefaultDepth = 200;

    explicit CommandStack(std::size_t maxDepth = kDefaultDepth) : maxDepth_{maxDepth} {}

    /// Apply a command and push it. Discards any redo history, and coalesces
    /// with the previous command when their merge keys match.
    void execute(model::Project& project, CommandPtr command);

    [[nodiscard]] bool canUndo() const noexcept { return position_ > 0; }
    [[nodiscard]] bool canRedo() const noexcept { return position_ < commands_.size(); }

    bool undo(model::Project& project);
    bool redo(model::Project& project);

    /// What undo/redo would do next, for menu labels. Empty when unavailable.
    [[nodiscard]] std::string undoDescription() const;
    [[nodiscard]] std::string redoDescription() const;

    /// Newest first -- the order a History panel lists them.
    [[nodiscard]] std::vector<std::string> history() const;

    /// Ends the current merge group, so the next command starts a new undo step
    /// even if its key matches. Called when a drag finishes or focus changes.
    void breakMerge() noexcept { mergeBroken_ = true; }

    /// Fold everything executed while one of these is alive into one undo step,
    /// whatever the commands' own merge keys say.
    ///
    /// Merge keys cannot do this on their own, and should not: they name *what*
    /// is being changed, so that dragging one clip's opacity coalesces and a
    /// change to a different clip does not. Setting the opacity of five
    /// selected clips is five different keys and one gesture, and undoing it a
    /// clip at a time is not what anybody meant by it.
    ///
    /// Safe to nest -- only the outermost one opens and closes the step -- and
    /// safe around an operation that fails and executes nothing, which simply
    /// contributes no command to the group.
    class Group {
    public:
        explicit Group(CommandStack& stack) : stack_{&stack} { stack_->beginGroup(); }
        ~Group() { stack_->endGroup(); }
        Group(const Group&) = delete;
        Group& operator=(const Group&) = delete;
        Group(Group&&) = delete;
        Group& operator=(Group&&) = delete;

    private:
        CommandStack* stack_;
    };

    void beginGroup() noexcept;
    void endGroup() noexcept;

    void clear();

    /// Remember that the project as it stands now is what is on disk.
    ///
    /// A position in the history rather than a flag, so that undoing back to
    /// the saved state reports the project as unmodified again -- which is what
    /// it is, and a "modified" marker that will not go away is one people stop
    /// reading.
    void markSaved() noexcept;

    /// Whether the project differs from what was last saved.
    ///
    /// True when there is no saved position to compare against, which covers a
    /// project that has never been saved and one whose saved state has become
    /// unreachable -- dropped off the end of the history, or stranded on a
    /// branch that a new command discarded. Claiming "unmodified" in those
    /// cases would be a guess, and the cost of guessing wrong is somebody's
    /// work.
    [[nodiscard]] bool isModified() const noexcept;

    [[nodiscard]] std::size_t depth() const noexcept { return commands_.size(); }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    [[nodiscard]] std::size_t snapshotBytes() const;

private:
    std::vector<CommandPtr> commands_;
    /// Number of commands currently applied; also the index of the next redo.
    std::size_t position_{0};
    std::size_t maxDepth_;
    bool mergeBroken_{true};
    /// How many `Group`s are open, and whether one has taken a command yet.
    /// Depth rather than a flag so that a push made inside another one -- an
    /// operation that groups internally, called from a panel that is grouping
    /// too -- does not close the outer step early.
    std::size_t groupDepth_{0};
    bool groupJoined_{false};
    std::size_t savedPosition_{0};
    /// False when the saved state is no longer anywhere in this history.
    bool savedKnown_{false};
};

}  // namespace zaro::edit
