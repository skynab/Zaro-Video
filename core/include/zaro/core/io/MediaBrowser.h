#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "zaro/core/Error.h"

namespace zaro::io {

/// One row of a folder listing.
struct FolderEntry {
    std::string name;
    std::string path;
    bool isFolder{false};
    std::uint64_t bytes{0};
};

/// What a browser shows for a folder: its subfolders and the media in it.
///
/// **Folders first, then files, each sorted by name.** That is the order every
/// file manager uses, and a browser that invented its own would be one people
/// have to read rather than scan.
///
/// **Media only.** A card of footage also holds sidecar files, thumbnails and
/// a manifest, none of which can be imported; listing them would make finding
/// the footage harder, which is the one job here.
///
/// **Hidden files stay hidden.** A dot file is one somebody's tools made, not
/// one they shot.
[[nodiscard]] Result<std::vector<FolderEntry>> listFolder(const std::string& path);

/// Whether a path looks like something this program can open.
///
/// By extension, not by opening it: a browser that probed every file in a
/// folder of camera media would take a minute to draw. Being wrong here costs
/// an import that fails with a clear message; being slow costs the feature.
[[nodiscard]] bool looksLikeMedia(const std::string& path);

}  // namespace zaro::io
