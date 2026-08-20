#include "zaro/core/model/Graphic.h"

#include <cstring>

namespace zaro::model {

const char* toString(GraphicKind kind) noexcept {
    switch (kind) {
        case GraphicKind::None:
            return "none";
        case GraphicKind::Rectangle:
            return "rectangle";
        case GraphicKind::Ellipse:
            return "ellipse";
        case GraphicKind::Text:
            return "text";
    }
    return "none";
}

GraphicKind graphicKindFromString(const char* name) noexcept {
    if (name == nullptr) {
        return GraphicKind::None;
    }
    if (std::strcmp(name, "rectangle") == 0) {
        return GraphicKind::Rectangle;
    }
    if (std::strcmp(name, "ellipse") == 0) {
        return GraphicKind::Ellipse;
    }
    if (std::strcmp(name, "text") == 0) {
        return GraphicKind::Text;
    }
    return GraphicKind::None;
}

}  // namespace zaro::model
