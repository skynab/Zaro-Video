#include "Document.h"

#include <filesystem>
#include <system_error>
#include <utility>

namespace zaro::app {

bool Document::lockingEnabled() noexcept {
    return locking_;
}

void Document::setLockingEnabled(bool enabled) noexcept {
    locking_ = enabled;
}

Result<Document::Opened> Document::read(const std::string& path, Sharing sharing) const {
    auto loaded = io::loadProject(path);
    if (!loaded) {
        return loaded.error();
    }
    if (loaded->project.findSequence(loaded->project.activeSequence()) == nullptr) {
        return Error{ErrorCode::InvalidData, "that project has no active sequence"};
    }

    Opened opened;
    if (auto held = io::readLock(path); lockingEnabled() && held && !io::isOurs(*held)) {
        const bool free = io::isStale(*held);
        if (!free && sharing == Sharing::Exclusive) {
            return Error{ErrorCode::InvalidData,
                         held->user + " has this project open on " + held->host};
        }
        opened.readOnly = !free && sharing == Sharing::ReadOnly;
    }
    opened.loaded = std::move(*loaded);
    return opened;
}

void Document::adopt(model::Project project, io::LoadedProject loaded, std::string path) {
    project_ = std::move(project);
    loaded_ = std::move(loaded);
    path_ = std::move(path);
    readOnly_ = false;
    // The history belongs to the project that has just gone.
    commands_.clear();
    commands_.markSaved();
}

Status Document::save() {
    if (path_.empty()) {
        return Error{ErrorCode::InvalidData, "this project has never been saved"};
    }
    if (readOnly_) {
        const std::string who = heldBy();
        return Error{ErrorCode::InvalidData,
                     "read only: " + (who.empty() ? std::string{"somebody else"} : who) +
                         " has this project open; save a new version to keep your work"};
    }
    if (Status written = io::saveProject(project_, path_, loaded_.unknown); !written) {
        return written;
    }
    commands_.markSaved();
    // The recovery file describes work that is now in the project itself. Left
    // behind, it would be offered on the next open as though it were newer,
    // which is an alarming thing to be asked about a file that is correct.
    std::error_code code;
    std::filesystem::remove(io::autosavePath(path_), code);
    return {};
}

Result<std::string> Document::saveNewVersion() {
    if (path_.empty()) {
        // Nowhere to count from. Asking where to put it is the honest answer,
        // and after that there is a version one to count from.
        return Error{ErrorCode::InvalidData, "save this project once before versioning it"};
    }
    const std::string next = io::nextVersionPath(path_);
    if (Status written = io::saveProject(project_, next, loaded_.unknown); !written) {
        return written.error();
    }
    path_ = next;
    // A new version is a different file, which nobody else has open.
    readOnly_ = false;
    commands_.markSaved();
    std::error_code code;
    std::filesystem::remove(io::autosavePath(next), code);
    return next;
}

Status Document::saveAs(std::string path) {
    path_ = std::move(path);
    readOnly_ = false;
    return save();
}

void Document::autosave() const {
    if (path_.empty() || !commands_.isModified()) {
        return;
    }
    static_cast<void>(io::saveProject(project_, io::autosavePath(path_), loaded_.unknown));
}

std::string Document::heldBy() const {
    if (path_.empty()) {
        return {};
    }
    auto held = io::readLock(path_);
    if (!held || io::isOurs(*held) || io::isStale(*held)) {
        return {};
    }
    return held->user + " on " + held->host;
}

void Document::takeLock() const {
    if (!lockingEnabled() || readOnly_ || path_.empty()) {
        return;
    }
    // Advisory, so a volume that will not take one is not a reason to refuse to
    // work: the failure is ignored on purpose.
    static_cast<void>(io::writeLock(path_, io::thisProcess()));
}

void Document::releaseLock() const {
    if (!path_.empty() && !readOnly_) {
        static_cast<void>(io::removeLock(path_));
    }
}

}  // namespace zaro::app
