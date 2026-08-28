// Where a project's files are: finding them again, gathering them up, and
// making small copies to cut against.
//
// All three change the project's media rather than its cut, which is why they
// end by asking the bin to redraw rather than the monitor. See
// PreviewWindow::afterMediaChange.
#pragma once

#include <cstdint>

#include "zaro/core/Error.h"
#include "zaro/core/io/ProjectIo.h"
#include "zaro/core/io/Relink.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"

#include "Context.h"

namespace zaro::app::commands {

/// Find the project's missing files under a folder and point it at them.
///
/// Everything found is relinked, and what is not found is reported: a
/// dialog per file would be a dialog per file, and the report says exactly
/// which ones were matched only by their name.
Result<io::RelinkReport> relinkMedia(const Context& context, const std::string& root);

/// Gather the project's media into one folder and point it at the copies.
///
/// The copying and the relinking are one action here and two underneath:
/// the copy is a filesystem change nothing can undo, and the relink is an
/// edit like any other. Undo therefore puts the project back on the
/// originals and leaves the copies where they are, which is the honest
/// half to be able to take back.
Result<io::ConsolidateReport> consolidateMedia(const Context& context,
                                               const std::string& destination);

/// Make a proxy for one media reference and attach it.
///
/// Beside the original by default, named after it. Not in a temporary
/// folder: a proxy the system might delete under a project is worse than
/// no proxy, and one nobody can find is one everybody remakes.
Result<platform::ffmpeg::ProxySummary> buildProxy(const Context& context, model::MediaRefId mediaId,
                                                  std::int32_t width = 960);

}  // namespace zaro::app::commands
