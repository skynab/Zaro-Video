// Motion graphics templates: a title saved out of one cut and dropped into
// another.
#pragma once

#include "zaro/core/Error.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"

#include "Context.h"

namespace zaro::app::commands {

/// Save the selected graphic on its own, so it can be used again.
Status saveGraphicTemplate(const Context& context, const std::string& path);

/// Drop a saved graphic in at the playhead, on the selected track.
Result<model::ClipId> placeGraphicTemplate(const Context& context, const std::string& path,
                                           const time::RationalTime& duration);

}  // namespace zaro::app::commands
