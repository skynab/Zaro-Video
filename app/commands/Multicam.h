// Lining up the angles of a multicam clip.
#pragma once

#include <string>
#include <vector>

#include "zaro/core/Error.h"

#include "Context.h"

namespace zaro::app::commands {

/// What syncing the angles came to.
///
/// A report rather than a Status, because "some of them" is the usual answer
/// and the interesting part is which ones did not: an angle that was recorded
/// on a camera whose clock was never set has no timecode to line up by, and
/// saying so by name is the difference between a feature that failed and one
/// that told somebody what to fix.
struct AngleSyncReport {
    std::int32_t synced{0};
    std::int32_t total{0};
    /// One line per angle that could not be synced, naming it and saying why.
    std::vector<std::string> skipped;
};

/// Line the angles up, by their timecode or by what they sound like.
Result<AngleSyncReport> syncAngles(const Context& context, bool byEar);

}  // namespace zaro::app::commands
