#pragma once

#include <memory>
#include <string>

#include "zaro/core/Error.h"
#include "zaro/core/model/Project.h"

namespace zaro::io {

/// The schema version written by this build. Bumped when the meaning of an
/// existing field changes; adding a field does not need a bump, because
/// unknown fields survive a round trip (see UnknownFields).
inline constexpr int kProjectSchemaVersion = 1;

/// Fields this build did not recognise, carried through a load/save cycle.
///
/// Project files outlive the build that wrote them, and people run mixed
/// versions. Without this, opening a project in an older build and pressing
/// save silently deletes everything the newer build added -- effects, markers,
/// whatever it happens to be. Preserving the unrecognised parts costs a little
/// memory and turns data loss into a no-op.
///
/// Opaque on purpose: the JSON library stays an implementation detail of
/// core/io rather than leaking into everything that saves a project.
class UnknownFields;

struct LoadedProject {
    model::Project project;
    std::shared_ptr<const UnknownFields> unknown;
};

[[nodiscard]] Result<LoadedProject> loadProjectFromString(const std::string& text);
[[nodiscard]] Result<LoadedProject> loadProject(const std::string& path);

/// Pass back the `unknown` from a load to preserve anything this build does not
/// understand. Omit it when writing a project built in memory.
[[nodiscard]] Result<std::string> saveProjectToString(
    const model::Project& project, const std::shared_ptr<const UnknownFields>& unknown = nullptr);
[[nodiscard]] Status saveProject(const model::Project& project, const std::string& path,
                                 const std::shared_ptr<const UnknownFields>& unknown = nullptr);

}  // namespace zaro::io
