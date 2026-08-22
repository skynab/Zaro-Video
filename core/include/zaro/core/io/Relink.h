#pragma once

#include <string>
#include <vector>

#include "zaro/core/Error.h"
#include "zaro/core/model/Project.h"

namespace zaro::io {

/// One missing file and where it seems to have gone.
struct RelinkMatch {
    model::MediaRefId media;
    /// The path the project has, which no longer exists.
    std::string was;
    std::string found;
    /// Whether the candidate's content matched, or only its name.
    ///
    /// Reported rather than used to decide, because both answers are useful and
    /// they are useful in different ways: a content match is a file somebody can
    /// accept without looking, and a name match is a file they should look at.
    bool byContent{false};
};

struct RelinkReport {
    std::vector<RelinkMatch> matches;
    /// Media that is missing and was not found under the folder searched.
    std::vector<model::MediaRefId> stillMissing;
    /// Files looked at, so a search that found nothing can say whether it
    /// looked anywhere.
    int examined{0};
};

/// Look for the project's missing media under a folder.
///
/// **Missing only.** A file that is where the project says it is is not
/// something to go looking for, and offering to relink it would be offering to
/// break it.
///
/// **By name first, then by content.** Filename is what narrows a folder of
/// thousands to a handful in one pass; the digest is what says which of the
/// handful is the file. Searching by content alone would mean reading every
/// file under the folder, which on a card of camera media is minutes.
///
/// **Content wins over position.** Where several files share the name, the one
/// whose digest matches is chosen; failing that, the first in sorted order, so
/// two runs over the same folder answer the same thing.
///
/// **Nothing is applied.** The report says what was found and the caller
/// decides -- an automatic relink that picked the wrong take is the kind of
/// mistake nobody notices until the export.
[[nodiscard]] Result<RelinkReport> findRelinks(const model::Project& project,
                                               const std::string& root);

/// One file gathered into the destination folder.
struct ConsolidatedFile {
    model::MediaRefId media;
    std::string from;
    std::string to;
    /// True when the file was already inside the destination and was left
    /// where it was rather than copied beside itself.
    bool alreadyThere{false};
    std::uint64_t bytes{0};
};

struct ConsolidateReport {
    std::vector<ConsolidatedFile> files;
    /// Media that could not be copied because it is not where the project says
    /// it is. Relink first; consolidating a file nobody can find would mean
    /// writing an empty one and calling it gathered.
    std::vector<model::MediaRefId> missing;
    std::uint64_t bytes{0};
};

/// Copy every file the project uses into one folder.
///
/// **Copies, never moves.** The originals are somebody's rushes. A consolidate
/// that moved them would be a consolidate that lost them the first time it was
/// pointed at the wrong folder.
///
/// **Names that collide are suffixed, not overwritten.** Two cards both
/// containing `C0001.MP4` is the ordinary case, not the exotic one, and the
/// second one silently replacing the first is the worst thing this could do.
///
/// **A file already inside the destination stays where it is.** Consolidating
/// twice should not produce `shot-2.mov`, and copying a file beside itself is
/// how a project's folder doubles in size for no reason.
///
/// **Nothing is relinked here.** The report says what landed where and the
/// caller points the project at it, for the same reason relinking is two steps:
/// the filesystem work and the edit are separately undoable, and only one of
/// them is.
///
/// Proxies are not gathered yet, and a media reference that has one keeps
/// pointing at wherever that proxy is.
[[nodiscard]] Result<ConsolidateReport> consolidate(const model::Project& project,
                                                    const std::string& destination);

}  // namespace zaro::io
