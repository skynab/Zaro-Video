#pragma once

#include <cstdint>
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

/// A hash of everything about a thing that a project file records about it.
///
/// Two clips with the same fingerprint produce the same picture; two that
/// differ in any way the file records differ here too. That is the whole
/// guarantee, and it is why these live beside the writer rather than beside
/// the renderer: they run the *same* encoder the file does, so a field added
/// to `Clip` and written to disk is in the fingerprint the moment it is
/// written, with nothing to remember to update.
///
/// The alternative -- a hand-written list of the fields that matter to a
/// render -- fails silently and late: the field is added, the fingerprint does
/// not change, and the render cache serves a frame from before the change.
/// Nobody would look for that in the cache.
///
/// The value is stable within a build and is not written to disk. It is a
/// change detector, not an identifier.
[[nodiscard]] std::uint64_t fingerprint(const model::Clip& clip);
[[nodiscard]] std::uint64_t fingerprint(const model::Transition& transition);
[[nodiscard]] std::uint64_t fingerprint(const model::MediaRef& media);
[[nodiscard]] std::uint64_t fingerprint(const model::CaptionTrack& captions);

}  // namespace zaro::io
