#include "zaro/core/io/ProjectLock.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <process.h>
#include <windows.h>
#else
#include <csignal>

#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "zaro/core/Environment.h"

namespace zaro::io {
namespace {

using json = nlohmann::json;

[[nodiscard]] std::string hostName() {
#if defined(_WIN32)
    const std::optional<std::string> name = environmentValue("COMPUTERNAME");
    return name.has_value() ? *name : "unknown";
#else
    std::array<char, 256> buffer{};
    if (::gethostname(buffer.data(), buffer.size() - 1) == 0) {
        return std::string{buffer.data()};
    }
    return "unknown";
#endif
}

[[nodiscard]] std::string userName() {
    for (const char* variable : {"USER", "USERNAME", "LOGNAME"}) {
        if (const std::optional<std::string> value = environmentValue(variable);
            value.has_value() && !value->empty()) {
            return *value;
        }
    }
    return "somebody";
}

[[nodiscard]] bool processIsRunning(std::int64_t pid) {
    if (pid <= 0) {
        return false;
    }
#if defined(_WIN32)
    if (pid > static_cast<std::int64_t>(std::numeric_limits<DWORD>::max())) {
        return false;  // not a process id this machine could ever have issued
    }
    // PROCESS_QUERY_LIMITED_INFORMATION rather than the full right: it is
    // granted across integrity levels, so a lock left by an elevated session
    // still answers instead of looking dead.
    const HANDLE handle =
        ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) {
        // Access denied means the process is there and belongs to somebody
        // else, which is still alive. Anything else -- ERROR_INVALID_PARAMETER
        // for a pid nobody holds -- means it is gone.
        return ::GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD exitCode = 0;
    // A pid is reused once the last handle to the process closes, so a handle
    // that opens can still name something that has exited.
    const bool running = ::GetExitCodeProcess(handle, &exitCode) != 0 && exitCode == STILL_ACTIVE;
    ::CloseHandle(handle);
    return running;
#else
    // Signal zero: the permission and existence checks run, nothing is sent.
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    // EPERM means it exists and belongs to somebody else, which is still alive.
    return errno == EPERM;
#endif
}

}  // namespace

std::string lockPath(const std::string& projectPath) {
    return projectPath + ".lock";
}

ProjectLock thisProcess() {
    ProjectLock lock;
    lock.user = userName();
    lock.host = hostName();
#if defined(_WIN32)
    lock.pid = _getpid();
#else
    lock.pid = ::getpid();
#endif
    lock.openedAt = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return lock;
}

Result<ProjectLock> readLock(const std::string& projectPath) {
    std::ifstream file{lockPath(projectPath), std::ios::binary};
    if (!file) {
        return Error{ErrorCode::NotFound, "nobody has " + projectPath + " open"};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();

    const json parsed = json::parse(buffer.str(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        // A lock file that cannot be read is treated as somebody's, not as
        // nobody's: the safe reading of "I do not understand this" is that
        // another program wrote it.
        ProjectLock unknown;
        unknown.user = "somebody";
        unknown.host = "an unknown machine";
        return unknown;
    }
    ProjectLock lock;
    lock.user = parsed.value("user", std::string{"somebody"});
    lock.host = parsed.value("host", std::string{"an unknown machine"});
    lock.pid = parsed.value("pid", std::int64_t{0});
    lock.openedAt = parsed.value("openedAt", std::int64_t{0});
    return lock;
}

Status writeLock(const std::string& projectPath, const ProjectLock& lock) {
    const json out{
        {"user", lock.user}, {"host", lock.host}, {"pid", lock.pid}, {"openedAt", lock.openedAt}};
    std::ofstream file{lockPath(projectPath), std::ios::binary | std::ios::trunc};
    if (!file) {
        // Not being able to write one is not a reason to refuse to open the
        // project: read-only volumes exist, and a lock is advisory.
        return Error{ErrorCode::Io, "cannot write a lock beside " + projectPath};
    }
    file << out.dump(2);
    file.flush();
    return file ? Status{} : Error{ErrorCode::Io, "failed while writing the lock"};
}

Status removeLock(const std::string& projectPath, bool force) {
    if (!force) {
        auto existing = readLock(projectPath);
        if (!existing) {
            return {};  // nothing to remove
        }
        if (!isOurs(*existing)) {
            return Error{ErrorCode::InvalidData, "that lock belongs to somebody else"};
        }
    }
    std::error_code code;
    std::filesystem::remove(lockPath(projectPath), code);
    return code ? Status{Error{ErrorCode::Io, "cannot remove the lock: " + code.message()}}
                : Status{};
}

bool isStale(const ProjectLock& lock) {
    if (lock.host != hostName()) {
        return false;
    }
    return !processIsRunning(lock.pid);
}

bool isOurs(const ProjectLock& lock) {
    const ProjectLock mine = thisProcess();
    return lock.host == mine.host && lock.pid == mine.pid;
}

}  // namespace zaro::io
