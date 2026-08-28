// Operations that change the shape of the cut rather than the look of it.
#pragma once

#include <cstdint>

#include "zaro/core/Error.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/core/render/SceneDetect.h"

#include "Context.h"

namespace zaro::app::commands {

/// Pin the selected clip to one on a lower track, or to nothing.
Result<model::ClipId> pinTo(const Context& context, model::ClipId host);

/// Cut the selected clip where the picture changes.
///
/// Returns how many cuts were made, so the self-test can say what happened
/// without a dialog. Zero is a perfectly good answer: a single continuous
/// take has no scene changes in it, and reporting one would be worse than
/// reporting none.
std::int32_t detectScenes(const Context& context, const Progress& tell);

/// Pin the selected clip to whatever is under it at the playhead.
///
/// "Under" means the topmost audible video track below the selected one, since
/// tracks are listed bottom-up and a hidden track is not what somebody means.
Result<model::ClipId> pinToClipBelow(const Context& context);

/// Set the curve the sequence goes out through.
Status setDelivery(const Context& context, const model::Sequence::Output& output);

/// Point the selected clip at a different file, keeping the cut it was given.
Status replaceSelectedSource(const Context& context, model::MediaRefId media);

/// Make a subclip of what is marked in a file.
///
/// Numbered rather than named. Naming every subclip at the moment it is made is
/// a dialog between somebody and the thing they were doing; the bin lists them
/// under the file they came from, which is how they are found anyway.
Result<model::SubclipId> makeSubclip(const Context& context, model::MediaRefId source,
                                     const time::TimeRange& range);

/// Which frame of which file the selected clip is showing at the playhead.
struct MatchedFrame {
    model::MediaRefId media;
    time::RationalTime at;
};

/// Find the frame the picture is currently made from.
///
/// Fails rather than guesses when the playhead is not over the selected clip,
/// or when what it is over is generated or a nest -- neither has a frame of a
/// file to match back to.
Result<MatchedFrame> frameToMatch(const Context& context);

}  // namespace zaro::app::commands
