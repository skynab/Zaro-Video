#pragma once

#include <cstdint>
#include <string>

#include "zaro/core/Error.h"

namespace zaro::io {

/// Who has a project open, written beside it while they do.
///
/// A file rather than a database: the projects this has to work across live on
/// a shared volume, and the only thing every machine mounting that volume
/// agrees on is the filesystem. It is advisory -- nothing here can stop another
/// program writing the project -- and that is the honest scope: it prevents two
/// people quietly overwriting each other, not a determined one.
struct ProjectLock {
    std::string user;
    std::string host;
    std::int64_t pid{0};
    /// Seconds since the epoch, for saying how long ago rather than for
    /// deciding anything: clocks on two machines disagree, so age is shown to
    /// a person and never used to steal a lock.
    std::int64_t openedAt{0};

    friend bool operator==(const ProjectLock&, const ProjectLock&) = default;
};

/// Where the lock for a project lives: beside it, named after it.
[[nodiscard]] std::string lockPath(const std::string& projectPath);

/// This process, as a lock.
[[nodiscard]] ProjectLock thisProcess();

/// Read the lock beside a project. `NotFound` when there is none, which is the
/// ordinary case and not an error worth shouting about.
[[nodiscard]] Result<ProjectLock> readLock(const std::string& projectPath);

[[nodiscard]] Status writeLock(const std::string& projectPath, const ProjectLock& lock);

/// Remove the lock. Only removes one this process holds unless `force`, so a
/// crash somewhere else cannot be tidied away by accident.
[[nodiscard]] Status removeLock(const std::string& projectPath, bool force = false);

/// Whether a lock describes something that is no longer running.
///
/// **Only decidable on the same machine.** A process id means nothing on
/// another host, so a lock from elsewhere is never called stale however old it
/// looks -- the alternative is stealing a lock from somebody whose machine was
/// merely slow to answer, which is exactly the case this exists to prevent.
[[nodiscard]] bool isStale(const ProjectLock& lock);

/// Whether this lock is our own: same host, same process.
[[nodiscard]] bool isOurs(const ProjectLock& lock);

}  // namespace zaro::io
