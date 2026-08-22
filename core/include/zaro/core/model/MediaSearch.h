#pragma once

#include <string>

#include "zaro/core/model/Project.h"

namespace zaro::model {

/// Everything about a media reference that somebody might search for, as one
/// lowercased string.
///
/// **One function feeds both the search and the display**, because a search
/// that matches something the list does not show is a search whose results
/// look wrong, and a list that shows something the search cannot find is worse.
/// What is here is what is findable: the name, the filename and the folder it
/// sits in, the codecs, the frame size and rate, the duration in seconds, and
/// whatever notes somebody has written on it.
[[nodiscard]] std::string searchTextFor(const MediaRef& media);

/// Whether a media reference matches a query.
///
/// **Every word has to match, in any order.** "prores 1080" finds the ProRes
/// files at 1080 and not the ProRes files at 720 -- which is what somebody
/// typing two words means, and is not what matching the phrase would do. An
/// empty query matches everything, so a cleared search box shows the bin
/// rather than nothing.
[[nodiscard]] bool matchesSearch(const MediaRef& media, const std::string& query);

}  // namespace zaro::model
