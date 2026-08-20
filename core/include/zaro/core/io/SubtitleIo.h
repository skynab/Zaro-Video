#pragma once

#include <string>

#include "zaro/core/Error.h"
#include "zaro/core/model/Caption.h"

namespace zaro::io {

/// SubRip and WebVTT.
///
/// The two formats differ in a header, a decimal separator and a handful of
/// features nobody uses in an edit; one reader handles both, because refusing a
/// .vtt that is a .srt with dots in it would be pedantry rather than
/// correctness.
enum class SubtitleFormat { SubRip, WebVtt };

[[nodiscard]] Result<model::CaptionTrack> parseSubtitles(const std::string& text);
[[nodiscard]] Result<model::CaptionTrack> loadSubtitles(const std::string& path);

[[nodiscard]] std::string writeSubtitles(const model::CaptionTrack& captions,
                                         SubtitleFormat format);
[[nodiscard]] Status saveSubtitles(const model::CaptionTrack& captions, const std::string& path,
                                   SubtitleFormat format);

/// The format a path's extension implies. Unknown extensions are SubRip, which
/// is the more widely accepted of the two.
[[nodiscard]] SubtitleFormat formatForPath(const std::string& path);

}  // namespace zaro::io
