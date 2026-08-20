#pragma once

#include <string>

#include "zaro/core/Error.h"
#include "zaro/core/model/Project.h"

namespace zaro::io {

/// OpenTimelineIO interchange.
///
/// The format every other editor can read, which is what makes it worth more
/// than any single importer: an edit can leave here and come back, or arrive
/// from a system that has never heard of this one.
///
/// **An OTIO track is a sequence, not a set of placed clips.** Position is
/// implied by order and duration, and a hole in a track is a `Gap` — an object,
/// not an absence. Everything else here follows from that: exporting writes the
/// gaps out, importing accumulates the durations back into positions, and a
/// round trip has to survive both.

/// Write one sequence as an OTIO timeline.
[[nodiscard]] Result<std::string> writeOtio(const model::Project& project,
                                            model::SequenceId sequence);

/// Read an OTIO timeline into a project of its own.
///
/// A project rather than a sequence: an OTIO file names media by URL, and those
/// have to become media references somewhere. Merging them into an existing
/// project would mean matching against media already there, which is a
/// different decision and belongs to whoever is doing the importing.
[[nodiscard]] Result<model::Project> readOtio(const std::string& text);

[[nodiscard]] Status saveOtio(const model::Project& project, model::SequenceId sequence,
                              const std::string& path);
[[nodiscard]] Result<model::Project> loadOtio(const std::string& path);

}  // namespace zaro::io
