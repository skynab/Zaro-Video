#pragma once

#include <string>

#include "zaro/core/Error.h"
#include "zaro/core/model/Sequence.h"

namespace zaro::io {

/// A sequence's review comments, as something to send somebody.
///
/// **Markdown, not a private format.** What this is for is leaving a
/// conversation: a producer reading it in an email, a director printing it, an
/// assistant pasting it into a task list. Anything that needs importing back
/// is a worse answer to all three.
///
/// **Timecode, not seconds.** The number has to be typeable into whatever the
/// other person is watching in, and that is a timecode.
///
/// **Ordered by time, not by when they were written.** A review is walked
/// through in the order the picture happens.
///
/// **Resolved ones are kept, and marked.** "What did they ask for, and what did
/// we do" is the question a review list answers, and dropping the done ones
/// takes half the answer with it.
///
/// Markers with neither a note nor an author are working markers rather than
/// comments, and are left out: a review list padded with somebody's own
/// "check this" flags is one nobody reads twice.
[[nodiscard]] std::string reviewNotes(const model::Sequence& sequence,
                                      const std::string& title = {});

/// Write the same thing to a file.
[[nodiscard]] Status writeReviewNotes(const model::Sequence& sequence, const std::string& path,
                                      const std::string& title = {});

}  // namespace zaro::io
