#include "zaro/core/model/Transition.h"

#include <algorithm>
#include <cstring>

namespace zaro::model {

const char* toString(TransitionKind kind) noexcept {
    switch (kind) {
        case TransitionKind::Wipe:
            return "wipe";
        case TransitionKind::Slide:
            return "slide";
        case TransitionKind::CrossDissolve:
        default:
            return "crossDissolve";
    }
}

TransitionKind transitionKindFromString(const char* name) noexcept {
    // A name this build does not know falls back to a dissolve rather than
    // refusing the project: an unfamiliar transition is a cut somebody can
    // still watch, and the alternative is a file that will not open.
    if (name != nullptr) {
        for (const TransitionKind kind :
             {TransitionKind::CrossDissolve, TransitionKind::Wipe, TransitionKind::Slide}) {
            if (std::strcmp(name, toString(kind)) == 0) {
                return kind;
            }
        }
    }
    return TransitionKind::CrossDissolve;
}

const char* toString(TransitionDirection direction) noexcept {
    switch (direction) {
        case TransitionDirection::Left:
            return "left";
        case TransitionDirection::Down:
            return "down";
        case TransitionDirection::Up:
            return "up";
        case TransitionDirection::Right:
        default:
            return "right";
    }
}

bool transitionDirectionFromString(const char* name, TransitionDirection& out) noexcept {
    if (name == nullptr) {
        return false;
    }
    for (const TransitionDirection direction :
         {TransitionDirection::Right, TransitionDirection::Left, TransitionDirection::Down,
          TransitionDirection::Up}) {
        if (std::strcmp(name, toString(direction)) == 0) {
            out = direction;
            return true;
        }
    }
    return false;
}

double Transition::progressAt(const time::RationalTime& t) const {
    if (range.isEmpty()) {
        return 1.0;
    }
    const double elapsed = (t - range.start()).toSecondsDouble();
    const double total = range.duration().toSecondsDouble();
    if (total <= 0.0) {
        return 1.0;
    }
    return std::clamp(elapsed / total, 0.0, 1.0);
}

}  // namespace zaro::model
