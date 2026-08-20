#include "zaro/core/model/Caption.h"

#include <algorithm>

namespace zaro::model {

void CaptionTrack::add(const Caption& caption) {
    const auto at = std::lower_bound(captions_.begin(), captions_.end(), caption,
                                     [](const Caption& lhs, const Caption& rhs) {
                                         return lhs.range.start() < rhs.range.start();
                                     });
    captions_.insert(at, caption);
}

bool CaptionTrack::removeAt(std::size_t index) {
    if (index >= captions_.size()) {
        return false;
    }
    captions_.erase(captions_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

std::vector<const Caption*> CaptionTrack::at(const time::RationalTime& when) const {
    std::vector<const Caption*> showing;
    for (const Caption& caption : captions_) {
        if (caption.range.contains(when.rescaledTo(caption.range.start().rate()))) {
            showing.push_back(&caption);
        }
    }
    return showing;
}

}  // namespace zaro::model
