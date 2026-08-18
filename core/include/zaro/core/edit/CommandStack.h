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

    void clear();

    [[nodiscard]] std::size_t depth() const noexcept { return commands_.size(); }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    [[nodiscard]] std::size_t snapshotBytes() const;

private:
    std::vector<CommandPtr> commands_;
    /// Number of commands currently applied; also the index of the next redo.
    std::size_t position_{0};
    std::size_t maxDepth_;
    bool mergeBroken_{true};
};

}  // namespace zaro::edit
