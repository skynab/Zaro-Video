// Fitting music to a length.
#pragma once

#include "zaro/core/Error.h"
#include "zaro/core/render/Remix.h"

#include "Context.h"

namespace zaro::app::commands {

/// Fit the selected music clip to a length by taking a piece out of it.
///
/// The length wanted is the picture's: fitting music to a cut is the
/// errand, and asking for a number when the answer is on screen would be a
/// question with one sensible reply.
Result<render::RemixPlan> remixSelectedTo(const Context& context, double targetSeconds);

}  // namespace zaro::app::commands
