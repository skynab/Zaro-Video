#include "zaro/core/model/Mask.h"

#include <cstring>

namespace zaro::model {

const char* toString(MaskShape shape) noexcept {
    switch (shape) {
        case MaskShape::None:
            return "none";
        case MaskShape::Rectangle:
            return "rectangle";
        case MaskShape::Ellipse:
            return "ellipse";
        case MaskShape::Path:
            return "path";
    }
    return "none";
}

MaskShape maskShapeFromString(const char* name) noexcept {
    if (name == nullptr) {
        return MaskShape::None;
    }
    if (std::strcmp(name, "rectangle") == 0) {
        return MaskShape::Rectangle;
    }
    if (std::strcmp(name, "ellipse") == 0) {
        return MaskShape::Ellipse;
    }
    if (std::strcmp(name, "path") == 0) {
        return MaskShape::Path;
    }
    return MaskShape::None;
}

}  // namespace zaro::model
