#include "zaro/core/io/MediaBrowser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>

namespace zaro::io {
namespace {

/// What this program can open, by extension.
///
/// A list rather than a probe, and a short one: the point is to find the
/// footage on a card, not to be exhaustive about formats nobody shoots.
constexpr std::array<const char*, 25> kMediaExtensions{
    ".mov", ".mp4", ".m4v", ".mxf", ".avi", ".mkv", ".webm", ".mts", ".m2ts", ".braw", ".r3d",
    ".wav", ".aif", ".aiff", ".mp3", ".m4a", ".flac",
    // Stills. A photograph is footage that does not move: it is imported,
    // dragged, trimmed, graded and keyframed by the same code, so it belongs on
    // this list rather than behind a second kind of import.
    //
    // Only what this build actually decodes. HEIC is deliberately absent: the
    // list is a promise that a file will open, and offering one that then
    // fails to probe is worse than not offering it.
    ".png", ".jpg", ".jpeg", ".tif", ".tiff", ".bmp", ".webp", ".gif"};

[[nodiscard]] std::string lowered(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char letter : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(letter))));
    }
    return out;
}

}  // namespace

bool looksLikeMedia(const std::string& path) {
    const std::string extension = lowered(std::filesystem::path{path}.extension().string());
    return std::any_of(kMediaExtensions.begin(), kMediaExtensions.end(),
                       [&extension](const char* known) { return extension == known; });
}

Result<std::vector<FolderEntry>> listFolder(const std::string& path) {
    std::error_code code;
    if (!std::filesystem::is_directory(path, code)) {
        return Error{ErrorCode::NotFound, path + " is not a folder"};
    }

    std::vector<FolderEntry> folders;
    std::vector<FolderEntry> files;
    for (const auto& entry : std::filesystem::directory_iterator{
             path, std::filesystem::directory_options::skip_permission_denied, code}) {
        const std::string name = entry.path().filename().string();
        if (!name.empty() && name.front() == '.') {
            continue;  // somebody's tools made it, not their camera
        }
        FolderEntry row;
        row.name = name;
        row.path = entry.path().string();
        if (entry.is_directory(code)) {
            row.isFolder = true;
            folders.push_back(std::move(row));
            continue;
        }
        if (!entry.is_regular_file(code) || !looksLikeMedia(row.path)) {
            continue;
        }
        row.bytes = static_cast<std::uint64_t>(entry.file_size(code));
        files.push_back(std::move(row));
    }

    const auto byName = [](const FolderEntry& a, const FolderEntry& b) {
        return lowered(a.name) < lowered(b.name);
    };
    std::sort(folders.begin(), folders.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    folders.insert(folders.end(), std::make_move_iterator(files.begin()),
                   std::make_move_iterator(files.end()));
    return folders;
}

}  // namespace zaro::io
