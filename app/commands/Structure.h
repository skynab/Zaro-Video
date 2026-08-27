// Operations that change the shape of the cut rather than the look of it.
#pragma once

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

}  // namespace zaro::app::commands
