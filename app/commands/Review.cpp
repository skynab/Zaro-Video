// Notes left against the cut, and the list of them somebody is sent.

#include "Review.h"

#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectLock.h"
#include "zaro/core/io/ReviewNotes.h"

namespace zaro::app::commands {

/// Tick the comment under the playhead off, or put it back.
///
/// A toggle rather than two commands, because the mistake somebody makes
/// is ticking off the wrong one, and the fix for that has to be the same
/// keystroke again.
Result<bool> toggleCommentHere(const Context& context) {
    const model::Sequence* sequence = context.sequence();
    if (sequence == nullptr) {
        return Error{ErrorCode::InvalidData, "there is no sequence"};
    }
    const model::Marker* found = nullptr;
    for (const model::Marker& marker : sequence->markers()) {
        if (context.position >= marker.range.start() &&
            context.position < marker.range.endExclusive()) {
            found = &marker;
        }
    }
    if (found == nullptr) {
        return Error{ErrorCode::NotFound, "there is no comment at the playhead"};
    }
    const bool resolved = !found->resolved;
    auto built = edit::makeSetMarkerReview(
        context.project(), sequence->id(), found->id,
        found->author.empty() ? io::thisProcess().user : found->author, resolved);
    if (!built) {
        return built.error();
    }
    context.commands().execute(context.project(), std::move(*built));
    return resolved;
}

/// Write the comments out as something to send somebody.
Status writeReviewNotes(const Context& context, const std::string& path) {
    const model::Sequence* sequence = context.sequence();
    if (sequence == nullptr) {
        return Error{ErrorCode::InvalidData, "there is no sequence"};
    }
    return io::writeReviewNotes(*sequence, path);
}

}  // namespace zaro::app::commands
