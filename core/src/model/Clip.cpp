#include "zaro/core/model/Clip.h"

#include "zaro/core/Check.h"

namespace zaro::model {

time::RationalTime Clip::sourceTimeAt(const time::RationalTime& timelineTime) const {
    ZARO_CHECK(!timelineRange.isEmpty(), "sourceTimeAt on a zero-length clip");

    const time::RationalTime offset = timelineTime - timelineRange.start();
    if (timelineRange.duration() == sourceRange.duration()) {
        // The common case: same rate, normal speed. Stay in integer frames
        // rather than round-tripping through a ratio that can only lose.
        return sourceRange.start() + offset.rescaledTo(sourceRange.start().rate());
    }

    const time::Rational fraction = offset.toSeconds() / timelineRange.duration().toSeconds();
    const time::Rational into = sourceRange.duration().toSeconds() * fraction;
    return sourceRange.start() + time::RationalTime::fromSeconds(into, sourceRange.start().rate());
}

}  // namespace zaro::model
