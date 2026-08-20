#include "zaro/core/model/Clip.h"

#include "zaro/core/Check.h"

namespace zaro::model {

double Clip::speed() const {
    if (timelineRange.duration().toSeconds().isZero()) {
        return 1.0;
    }
    return (sourceRange.duration().toSeconds() / timelineRange.duration().toSeconds()).toDouble();
}

time::RationalTime Clip::sourceTimeAt(const time::RationalTime& timelineTime) const {
    ZARO_CHECK(!timelineRange.isEmpty(), "sourceTimeAt on a zero-length clip");

    const time::RationalTime offset = timelineTime - timelineRange.start();
    if (!reversed && timelineRange.duration() == sourceRange.duration()) {
        // The common case: same rate, normal speed, forwards. Stay in integer
        // frames rather than round-tripping through a ratio that can only lose.
        return sourceRange.start() + offset.rescaledTo(sourceRange.start().rate());
    }

    time::Rational fraction = offset.toSeconds() / timelineRange.duration().toSeconds();
    if (reversed) {
        // The last frame first. Measured from one frame inside the end, not
        // from the end itself: the out point is exclusive, and reading it would
        // be reading the frame after the clip.
        fraction = time::Rational::fromInt(1) - fraction;
        const time::Rational lastFrame =
            time::RationalTime{1, sourceRange.start().rate()}.toSeconds() /
            sourceRange.duration().toSeconds();
        fraction = fraction - lastFrame;
        if (fraction.isNegative()) {
            fraction = time::Rational{};
        }
    }
    const time::Rational into = sourceRange.duration().toSeconds() * fraction;
    return sourceRange.start() + time::RationalTime::fromSeconds(into, sourceRange.start().rate());
}

time::RationalTime Clip::timelineTimeOf(const time::RationalTime& sourceTime) const {
    ZARO_CHECK(!sourceRange.isEmpty(), "timelineTimeOf on a zero-length clip");

    const time::RationalTime offset = sourceTime - sourceRange.start();
    if (reversed) {
        const time::Rational fraction =
            time::Rational::fromInt(1) - (offset.toSeconds() / sourceRange.duration().toSeconds());
        const time::Rational into = timelineRange.duration().toSeconds() * fraction;
        return timelineRange.start() +
               time::RationalTime::fromSeconds(into, timelineRange.start().rate());
    }
    if (timelineRange.duration() == sourceRange.duration()) {
        // Same rate and normal speed: stay in integer frames rather than
        // round-tripping through a ratio that can only lose.
        return timelineRange.start() + offset.rescaledTo(timelineRange.start().rate());
    }

    const time::Rational fraction = offset.toSeconds() / sourceRange.duration().toSeconds();
    const time::Rational into = timelineRange.duration().toSeconds() * fraction;
    return timelineRange.start() +
           time::RationalTime::fromSeconds(into, timelineRange.start().rate());
}

double Clip::sourceSecondsAt(const time::RationalTime& timelineTime) const {
    ZARO_CHECK(!timelineRange.isEmpty(), "sourceSecondsAt on a zero-length clip");

    const double offset = (timelineTime - timelineRange.start()).toSecondsDouble();
    const double sourceStart = sourceRange.start().toSecondsDouble();
    const double sourceLength = sourceRange.duration().toSecondsDouble();
    if (reversed) {
        // Keyframes are glued to the picture, so a reversed clip's animation
        // runs backwards with it. Anything else would leave a fade landing on a
        // different frame than it was set on, which is exactly what storing
        // keyframes in source time exists to prevent.
        const double timelineLength = timelineRange.duration().toSecondsDouble();
        const double into = timelineLength > 0.0 ? (1.0 - (offset / timelineLength)) : 0.0;
        return sourceStart + (into * sourceLength);
    }
    if (timelineRange.duration() == sourceRange.duration()) {
        return sourceStart + offset;
    }
    const double rate = sourceLength / timelineRange.duration().toSecondsDouble();
    return sourceStart + (offset * rate);
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

double Clip::parameterValue(Param param) const {
    switch (param) {
        case Param::PositionX:
            return transform.positionX;
        case Param::PositionY:
            return transform.positionY;
        case Param::ScaleX:
            return transform.scaleX;
        case Param::ScaleY:
            return transform.scaleY;
        case Param::RotationDegrees:
            return transform.rotationDegrees;
        case Param::AnchorX:
            return transform.anchorX;
        case Param::AnchorY:
            return transform.anchorY;
        case Param::Opacity:
            return transform.opacity;
        case Param::GainDb:
            return gainDb;
        case Param::Pan:
            return pan;
        case Param::Temperature:
            return color.temperature;
        case Param::Tint:
            return color.tint;
        case Param::Exposure:
            return color.exposure;
        case Param::Contrast:
            return color.contrast;
        case Param::Saturation:
            return color.saturation;
    }
    return 0.0;
}

void Clip::setParameterValue(Param param, double value) {
    switch (param) {
        case Param::PositionX:
            transform.positionX = value;
            return;
        case Param::PositionY:
            transform.positionY = value;
            return;
        case Param::ScaleX:
            transform.scaleX = value;
            return;
        case Param::ScaleY:
            transform.scaleY = value;
            return;
        case Param::RotationDegrees:
            transform.rotationDegrees = value;
            return;
        case Param::AnchorX:
            transform.anchorX = value;
            return;
        case Param::AnchorY:
            transform.anchorY = value;
            return;
        case Param::Opacity:
            transform.opacity = value;
            return;
        case Param::GainDb:
            gainDb = value;
            return;
        case Param::Pan:
            pan = value;
            return;
        case Param::Temperature:
            color.temperature = value;
            return;
        case Param::Tint:
            color.tint = value;
            return;
        case Param::Exposure:
            color.exposure = value;
            return;
        case Param::Contrast:
            color.contrast = value;
            return;
        case Param::Saturation:
            color.saturation = value;
            return;
    }
}

double Clip::parameterAt(Param param, const time::RationalTime& timelineTime) const {
    const Curve* curve = animation.find(param);
    if (curve == nullptr || curve->empty()) {
        return parameterValue(param);
    }
    return curve->valueAtSeconds(sourceSecondsAt(timelineTime));
}

ColorCorrection Clip::colorAt(const time::RationalTime& timelineTime) const {
    if (animation.empty()) {
        return color;
    }
    const double seconds = sourceSecondsAt(timelineTime);
    ColorCorrection graded = color;
    graded.temperature =
        Curve::valueOr(animation.find(Param::Temperature), seconds, color.temperature);
    graded.tint = Curve::valueOr(animation.find(Param::Tint), seconds, color.tint);
    graded.exposure = Curve::valueOr(animation.find(Param::Exposure), seconds, color.exposure);
    graded.contrast = Curve::valueOr(animation.find(Param::Contrast), seconds, color.contrast);
    graded.saturation =
        Curve::valueOr(animation.find(Param::Saturation), seconds, color.saturation);
    return graded;
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
