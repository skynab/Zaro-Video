// Notes left against the cut, and the list of them somebody is sent.
#pragma once

#include "zaro/core/Error.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectLock.h"
#include "zaro/core/io/ReviewNotes.h"

#include "Context.h"

namespace zaro::app::commands {

/// Tick the comment under the playhead off, or put it back.
///
/// A toggle rather than two commands, because the mistake somebody makes
/// is ticking off the wrong one, and the fix for that has to be the same
/// keystroke again.
Result<bool> toggleCommentHere(const Context& context);

/// Write the comments out as something to send somebody.
Status writeReviewNotes(const Context& context, const std::string& path);

}  // namespace zaro::app::commands
