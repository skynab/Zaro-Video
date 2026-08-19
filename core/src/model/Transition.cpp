#include "zaro/core/model/Transition.h"

#include <algorithm>
#include <cstring>

namespace zaro::model {

const char* toString(TransitionKind kind) noexcept {
    switch (kind) {
        case TransitionKind::CrossDissolve:
        default:
            return "crossDissolve";
    }
}

TransitionKind transitionKindFromString(const char* name) noexcept {
    (void)name;
    // Only one kind so far. Named rather than assumed, so adding wipes and
    // pushes later does not silently reinterpret existing project files.
    return TransitionKind::CrossDissolve;
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
