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
/// Writes beside the file and renames over it, so an interrupted save leaves
/// the previous version intact rather than half of each.
[[nodiscard]] Status saveProject(const model::Project& project, const std::string& path,
                                 const std::shared_ptr<const UnknownFields>& unknown = nullptr);

/// One graphic saved on its own, to be dropped into another sequence or
/// another project: a motion graphics template.
///
/// **The same encoder the project file uses.** A template is a clip, and a
/// second serialiser for clips would be a second thing to remember whenever a
/// field is added -- the failure being silent, since a template written by a
/// forgetful encoder loads perfectly and quietly lacks whatever was left out.
///
/// **Only graphics.** A template referring to media would carry a path that
/// means nothing in the project it lands in, and a template that silently
/// arrived empty is worse than one that refused to be made.
///
/// What travels with it: the shape or text, the transform, every curve on the
/// clip, the responsive intro and outro, the effect stack, the mask and the
/// grade. What does not: its id, and where it sat.
[[nodiscard]] Status saveGraphicTemplate(const model::Clip& clip, const std::string& path);
[[nodiscard]] Result<model::Clip> loadGraphicTemplate(const std::string& path);

/// Where the recovery file for a project lives.
///
/// Beside it, and named after it. Not in a temporary directory: a recovery file
/// somewhere else is one nobody finds, and one the operating system may clear
/// out from under them. Not over the project itself either -- autosaving into
/// the file somebody last chose to save is making a decision they did not.
[[nodiscard]] std::string autosavePath(const std::string& projectPath);

/// Whether a usable recovery file sits beside this project and is newer than
/// it, which is the only case where offering one is honest: an older autosave
/// describes work the last real save already includes.
[[nodiscard]] bool hasNewerAutosave(const std::string& projectPath);

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
