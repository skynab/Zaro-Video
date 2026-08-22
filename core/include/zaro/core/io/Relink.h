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

}  // namespace zaro::io
