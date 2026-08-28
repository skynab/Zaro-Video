#include "zaro/core/io/Relink.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>

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

Result<ConsolidateReport> consolidate(const model::Project& project,
                                      const std::string& destination) {
    std::error_code code;
    std::filesystem::create_directories(destination, code);
    if (!std::filesystem::is_directory(destination, code)) {
        return Error{ErrorCode::Io, "cannot use " + destination + " as a folder"};
    }
    const std::filesystem::path into = std::filesystem::absolute(destination, code);

    ConsolidateReport report;
    // What the folder already holds, plus what this run has put there, so a
    // name is claimed once whether it arrived now or last time.
    std::set<std::string> taken;
    for (const auto& entry : std::filesystem::directory_iterator{into, code}) {
        taken.insert(entry.path().filename().string());
    }

    for (const model::MediaRef& media : project.media()) {
        if (media.path.empty()) {
            continue;
        }
        const std::filesystem::path from = std::filesystem::absolute(media.path, code);
        if (!std::filesystem::is_regular_file(from, code)) {
            report.missing.push_back(media.id);
            continue;
        }

        ConsolidatedFile gathered;
        gathered.media = media.id;
        gathered.from = from.string();
        gathered.bytes = static_cast<std::uint64_t>(std::filesystem::file_size(from, code));

        if (from.parent_path() == into) {
            gathered.to = from.string();
            gathered.alreadyThere = true;
            report.files.push_back(std::move(gathered));
            continue;
        }

        // A name nobody else has. The suffix goes before the extension so the
        // file is still recognisably a .mov to everything that looks at names.
        const std::string stem = from.stem().string();
        const std::string extension = from.extension().string();
        std::string name = stem + extension;
        for (int attempt = 2; taken.count(name) != 0; ++attempt) {
            name = stem + "-" + std::to_string(attempt) + extension;
        }
        const std::filesystem::path to = into / name;
        std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing,
                                   code);
        if (code) {
            return Error{ErrorCode::Io, "cannot copy " + from.string() + ": " + code.message()};
        }
        taken.insert(name);
        gathered.to = to.string();
        report.bytes += gathered.bytes;
        report.files.push_back(std::move(gathered));
    }
    return report;
}

}  // namespace zaro::io
