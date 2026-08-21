#include "zaro/core/model/AudioRole.h"

#include <cstring>

namespace zaro::model {

const char* toString(AudioRole role) noexcept {
    switch (role) {
        case AudioRole::Unassigned:
            return "unassigned";
        case AudioRole::Dialogue:
            return "dialogue";
        case AudioRole::Music:
            return "music";
        case AudioRole::Effects:
            return "effects";
        case AudioRole::Ambience:
            return "ambience";
    }
    return "unassigned";
}

std::span<const AudioRole> allAudioRoles() noexcept {
    static constexpr AudioRole kAll[] = {AudioRole::Unassigned, AudioRole::Dialogue,
                                         AudioRole::Music, AudioRole::Effects, AudioRole::Ambience};
    return kAll;
}

bool audioRoleFromString(const char* name, AudioRole& out) noexcept {
    if (name == nullptr) {
        return false;
    }
    for (const AudioRole candidate : allAudioRoles()) {
        if (std::strcmp(name, toString(candidate)) == 0) {
            out = candidate;
            return true;
        }
    }
    return false;
}

}  // namespace zaro::model
