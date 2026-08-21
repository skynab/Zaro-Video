#pragma once

#include <cstdint>
#include <span>

namespace zaro::model {

/// What a piece of sound is for.
///
/// A property of the clip rather than of the track it happens to be on: the
/// role belongs to the material, and material moves. Somebody who drags a line
/// of dialogue onto the music track has not made it music.
///
/// The list is short on purpose. These four are the ones every mix is built
/// out of and the ones an automatic decision can act on; a longer list would be
/// a taxonomy nobody maintains, with most clips left on whatever the default
/// happened to be.
enum class AudioRole : std::uint8_t {
    /// Not said. The honest default: a clip nobody has classified is not
    /// dialogue, and treating it as such would duck music under every stray
    /// sound in the timeline.
    Unassigned,
    Dialogue,
    Music,
    Effects,
    Ambience,
};

[[nodiscard]] const char* toString(AudioRole role) noexcept;
[[nodiscard]] bool audioRoleFromString(const char* name, AudioRole& out) noexcept;
[[nodiscard]] std::span<const AudioRole> allAudioRoles() noexcept;

}  // namespace zaro::model
