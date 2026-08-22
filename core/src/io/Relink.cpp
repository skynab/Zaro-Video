#include "zaro/core/io/Relink.h"

#include <algorithm>
#include <filesystem>
#include <map>

#include "zaro/core/media/Waveform.h"

namespace zaro::io {
namespace {

/// How deep to go, and how many files to look at.
///
/// A bound rather than a promise to search everything: pointed at a home
/// directory or a mounted server, an unbounded walk is an application that has
/// stopped responding. The numbers are large enough for a card, a project
/// folder or a drive of rushes.
constexpr int kMaxDepth = 12;
constexpr int kMaxExamined = 200000;

}  // namespace

Result<RelinkReport> findRelinks(const model::Project& project, const std::string& root) {
    std::error_code code;
    if (!std::filesystem::is_directory(root, code)) {
        return Error{ErrorCode::NotFound, root + " is not a folder"};
    }

    // What is actually missing, keyed by the filename to look for.
    std::multimap<std::string, const model::MediaRef*> wanted;
    for (const model::MediaRef& media : project.media()) {
        if (media.path.empty() || std::filesystem::exists(media.path, code)) {
            continue;
        }
        wanted.emplace(std::filesystem::path{media.path}.filename().string(), &media);
    }

    RelinkReport report;
    if (wanted.empty()) {
        return report;
    }

    std::map<std::string, std::vector<std::string>> candidates;
    auto walk = std::filesystem::recursive_directory_iterator{
        root, std::filesystem::directory_options::skip_permission_denied, code};
    if (code) {
        return Error{ErrorCode::Io, "cannot read " + root + ": " + code.message()};
    }
    for (auto entry = walk; entry != std::filesystem::recursive_directory_iterator{}; ++entry) {
        if (report.examined >= kMaxExamined) {
            break;
        }
        if (entry.depth() >= kMaxDepth) {
            entry.disable_recursion_pending();
        }
        if (!entry->is_regular_file(code)) {
            continue;
        }
        ++report.examined;
        const std::string name = entry->path().filename().string();
        if (wanted.count(name) != 0) {
            candidates[name].push_back(entry->path().string());
        }
    }

    for (const auto& [name, media] : wanted) {
        const auto found = candidates.find(name);
        if (found == candidates.end() || found->second.empty()) {
            report.stillMissing.push_back(media->id);
            continue;
        }
        // Sorted so the fallback -- when no digest matches -- is the same on
        // every run rather than whatever order the filesystem handed back.
        std::vector<std::string> paths = found->second;
        std::sort(paths.begin(), paths.end());

        RelinkMatch match;
        match.media = media->id;
        match.was = media->path;
        match.found = paths.front();

        const std::string wantedDigest = media->contentDigest;
        if (!wantedDigest.empty()) {
            for (const std::string& path : paths) {
                if (auto digest = media::contentDigest(path); digest && *digest == wantedDigest) {
                    match.found = path;
                    match.byContent = true;
                    break;
                }
            }
        }
        report.matches.push_back(std::move(match));
    }
    return report;
}

}  // namespace zaro::io
