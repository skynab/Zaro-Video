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

double Clip::sourceSecondsAt(const time::RationalTime& timelineTime) const {
    ZARO_CHECK(!timelineRange.isEmpty(), "sourceSecondsAt on a zero-length clip");

    const double offset = (timelineTime - timelineRange.start()).toSecondsDouble();
    const double sourceStart = sourceRange.start().toSecondsDouble();
    if (timelineRange.duration() == sourceRange.duration()) {
        return sourceStart + offset;
    }
    const double speed =
        sourceRange.duration().toSecondsDouble() / timelineRange.duration().toSecondsDouble();
    return sourceStart + (offset * speed);
}

Transform Clip::transformAt(const time::RationalTime& timelineTime) const {
    if (animation.empty()) {
        return transform;
    }
    const double seconds = sourceSecondsAt(timelineTime);
    Transform animated = transform;
    animated.positionX =
        Curve::valueOr(animation.find(Param::PositionX), seconds, transform.positionX);
    animated.positionY =
        Curve::valueOr(animation.find(Param::PositionY), seconds, transform.positionY);
    animated.scaleX = Curve::valueOr(animation.find(Param::ScaleX), seconds, transform.scaleX);
    animated.scaleY = Curve::valueOr(animation.find(Param::ScaleY), seconds, transform.scaleY);
    animated.rotationDegrees =
        Curve::valueOr(animation.find(Param::RotationDegrees), seconds, transform.rotationDegrees);
    animated.anchorX = Curve::valueOr(animation.find(Param::AnchorX), seconds, transform.anchorX);
    animated.anchorY = Curve::valueOr(animation.find(Param::AnchorY), seconds, transform.anchorY);
    animated.opacity = Curve::valueOr(animation.find(Param::Opacity), seconds, transform.opacity);
    return animated;
}

double Clip::gainDbAt(const time::RationalTime& timelineTime) const {
    if (animation.empty()) {
        return gainDb;
    }
    return Curve::valueOr(animation.find(Param::GainDb), sourceSecondsAt(timelineTime), gainDb);
}

double Clip::panAt(const time::RationalTime& timelineTime) const {
    if (animation.empty()) {
        return pan;
    }
    return Curve::valueOr(animation.find(Param::Pan), sourceSecondsAt(timelineTime), pan);
}

}  // namespace zaro::model
