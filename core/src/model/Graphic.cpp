#include "zaro/core/model/Graphic.h"

#include <cstring>
#include <string>

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

std::string autoNameFor(const Graphic& graphic) {
    const std::string kind = toString(graphic.kind);
    if (graphic.kind != GraphicKind::Text || graphic.text.empty()) {
        return kind;
    }
    std::string first = graphic.text.substr(0, graphic.text.find('\n'));
    // Trailing blanks would make two names that read identically compare
    // differently, which matters because the comparison is what decides
    // whether a name was typed by hand.
    while (!first.empty() &&
           (first.back() == ' ' || first.back() == '\r' || first.back() == '\t')) {
        first.pop_back();
    }
    if (first.empty()) {
        return kind;
    }
    constexpr std::size_t kNameLimit = 40;
    if (first.size() > kNameLimit) {
        first = first.substr(0, kNameLimit) + "\u2026";
    }
    return first;
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
