// The project this window has open: what it is, where it came from, and
// whether we may write to it.
//
// These five things travel together -- the project, the unknown fields it was
// loaded with, its path, its undo history and its lock -- and every operation
// on them touches most of them. Saving writes the project to the path with the
// unknown fields, marks the history saved and clears the recovery file; taking
// a version does the same to a different path and clears read-only. Spread
// across a window as five members, that was five things to remember in the
// right order, and the places that forgot one were quiet: a save that left a
// recovery file behind is offered back on the next open as though it were the
// newer work.
//
// Nothing here knows about a window. Reporting a failure and asking where to
// put a file are the window's, which is why the methods return Status and
// Result instead of putting a dialog up -- and why `read` hands back what it
// loaded rather than installing it.
#pragma once

#include <string>

#include "zaro/core/Error.h"
#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/io/ProjectIo.h"
#include "zaro/core/io/ProjectLock.h"
#include "zaro/core/model/Project.h"

namespace zaro::app {

class Document {
public:
    /// How to treat a project somebody else already has open.
    enum class Sharing : std::uint8_t {
        /// Take the lock if it is free or stale; refuse to open otherwise.
        Exclusive,
        /// Open it anyway, without saving over their work.
        ReadOnly,
        /// Their lock is stale or they have gone home: take it.
        TakeOver,
    };

    /// A project read off disk, and what its lock said.
    ///
    /// Handed back rather than installed, so the caller decides when what it is
    /// showing changes: refusing after the window has already been replaced
    /// would be worse than not opening at all.
    struct Opened {
        io::LoadedProject loaded;
        bool readOnly{false};
    };

    [[nodiscard]] model::Project& project() noexcept { return project_; }
    [[nodiscard]] const model::Project& project() const noexcept { return project_; }
    [[nodiscard]] edit::CommandStack& commands() noexcept { return commands_; }
    [[nodiscard]] const edit::CommandStack& commands() const noexcept { return commands_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] bool isReadOnly() const noexcept { return readOnly_; }
    [[nodiscard]] bool isModified() const { return commands_.isModified(); }

    /// Read a project and decide about its lock, changing nothing.
    [[nodiscard]] Result<Opened> read(const std::string& path, Sharing sharing) const;

    /// Take a project that has been read, or made.
    ///
    /// Clears the history and the read-only flag: both belong to the project
    /// that has just gone. Left set, read-only would follow somebody into a new
    /// project and refuse to save it, naming a person who has nothing to do
    /// with it.
    void adopt(model::Project project, io::LoadedProject loaded, std::string path);

    /// Write the project back where it came from.
    ///
    /// Fails rather than asks: a document with no path does not know where to
    /// put itself, and choosing is the caller's job.
    [[nodiscard]] Status save();

    /// Save as the next version beside this one, and carry on in it.
    ///
    /// Carrying on in the new file rather than staying in the old one is the
    /// point: a version is a line somebody draws under what they had, and the
    /// next hour's work belongs after the line. The previous file is left
    /// exactly as it was, which is the other half of the point.
    [[nodiscard]] Result<std::string> saveNewVersion();

    /// Point Save at a different file, and write it there.
    ///
    /// Saving somewhere else is exactly the way out of somebody else's lock, so
    /// it clears read-only rather than being refused by it -- the old advice
    /// was "save a new version to keep your work", which the program then would
    /// not let anybody do.
    [[nodiscard]] Status saveAs(std::string path);

    /// Write the recovery file, if there is anything to recover.
    ///
    /// Failures are silent on purpose. An autosave is something the program
    /// does on its own, and interrupting somebody mid-edit to report one is
    /// worse than the missing file -- the next explicit save reports the same
    /// problem at a moment they are expecting an answer.
    void autosave() const;

    void setPath(std::string path) { path_ = std::move(path); }

    /// Say whether this window may write over the project.
    ///
    /// Set by whoever opened it, from what the lock said. Adopting clears it.
    void setReadOnly(bool readOnly) noexcept { readOnly_ = readOnly; }

    /// Who else has this project, if anybody. Empty when it is ours or free.
    [[nodiscard]] std::string heldBy() const;

    void takeLock() const;
    void releaseLock() const;

    /// Locking is off unless asked for.
    ///
    /// A lock file beside a project is right for a shared volume and wrong for
    /// one person on a laptop, where the only lock ever seen is a stale one
    /// left by a crash. `ZARO_LOCKING=1` turns it back on.
    [[nodiscard]] static bool lockingEnabled() noexcept;
    static void setLockingEnabled(bool enabled) noexcept;

private:
    model::Project project_;
    io::LoadedProject loaded_;
    /// Where the project came from, and where Save writes.
    std::string path_;
    edit::CommandStack commands_;
    /// Somebody else has this project open, so it must not be written over.
    bool readOnly_{false};
    static inline bool locking_{false};
};

}  // namespace zaro::app
