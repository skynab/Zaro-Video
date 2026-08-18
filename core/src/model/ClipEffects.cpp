#include "zaro/core/model/ClipEffects.h"

#include <cstring>

namespace zaro::model {

const char* toString(BlendMode mode) noexcept {
    switch (mode) {
        case BlendMode::Add:
            return "add";
        case BlendMode::Multiply:
            return "multiply";
        case BlendMode::Screen:
            return "screen";
        case BlendMode::Normal:
        default:
            return "normal";
    }
}

BlendMode blendModeFromString(const char* name) noexcept {
    if (name == nullptr) {
        return BlendMode::Normal;
    }
    if (std::strcmp(name, "add") == 0) {
        return BlendMode::Add;
    }
    if (std::strcmp(name, "multiply") == 0) {
        return BlendMode::Multiply;
    }
    if (std::strcmp(name, "screen") == 0) {
        return BlendMode::Screen;
    }
    return BlendMode::Normal;
}

}  // namespace zaro::model
