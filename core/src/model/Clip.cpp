#include "zaro/core/model/Clip.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "zaro/core/Check.h"

namespace zaro::model {
namespace {

/// The live angle, or the first when the index is not one.
///
/// Falling back to the first rather than clamping: an index outside the list is
/// a data error, and clamping would answer "the last angle", which is an
/// arbitrary camera to pick. The first is where a multicam clip starts, so it
/// is the one recovery nobody has to reason about.
std::size_t liveAngle(const std::vector<Clip::Angle>& angles, std::int32_t requested) {
    return requested >= 0 && requested < static_cast<std::int32_t>(angles.size())
               ? static_cast<std::size_t>(requested)
               : 0;
}

}  // namespace

MediaRefId Clip::activeSource() const {
    if (angles.empty()) {
        return source;
    }
    return angles[liveAngle(angles, activeAngle)].media;
}

time::RationalTime Clip::activeSourceTimeAt(const time::RationalTime& timelineTime) const {
    const time::RationalTime base = sourceTimeAt(timelineTime);
    if (angles.empty()) {
        return base;
    }
    // The offset is in the angle's own material, so it is added after the
    // clip's own mapping -- which has already accounted for trims and speed.
    return base + angles[liveAngle(angles, activeAngle)].offset.rescaledTo(base.rate());
}

time::RationalTime Clip::activeBaseSourceTimeAt(const time::RationalTime& timelineTime) const {
    return activeBaseSourceTimeAt(timelineTime, sourceRange.start().rate());
}

time::RationalTime Clip::activeBaseSourceTimeAt(const time::RationalTime& timelineTime,
                                                const time::Rational& atRate) const {
    const time::RationalTime base = baseSourceTimeAt(timelineTime, atRate);
    if (angles.empty()) {
        return base;
    }
    return base + angles[liveAngle(angles, activeAngle)].offset.rescaledTo(base.rate());
}

bool Clip::isTimeRemapped() const {
    const Curve* remap = animation.find(Param::TimeRemap);
    return remap != nullptr && !remap->empty();
}

double Clip::speed() const {
    if (timelineRange.duration().toSeconds().isZero()) {
        return 1.0;
    }
    return (sourceRange.duration().toSeconds() / timelineRange.duration().toSeconds()).toDouble();
}

time::RationalTime Clip::sourceTimeAt(const time::RationalTime& timelineTime) const {
    const time::RationalTime base = baseSourceTimeAt(timelineTime);
    const Curve* remap = animation.find(Param::TimeRemap);
    if (remap == nullptr || remap->empty()) {
        return base;
    }
    // The curve is read at the *un-remapped* time, which is what makes it a
    // remap rather than a definition of itself. Everything else on the clip is
    // read at the un-remapped time too, so a fade still runs over the seconds
    // it was drawn over even where the picture is frozen -- which is what
    // somebody dragging an opacity ramp across a freeze means by it.
    const double seconds = remap->valueAtSeconds(sourceSecondsAt(timelineTime));
    // Never before the start of the file. A curve dragged below zero is a
    // request for frames that do not exist, and the first frame is the honest
    // answer; the alternative is a clip that silently stops drawing.
    //
    // To the nearest frame rather than truncated: the curve is continuous and
    // the media is not, and rounding down would show every frame a little late
    // and hold the last one of a ramp for two.
    const double rate = base.rate().toDouble();
    const auto frames = static_cast<std::int64_t>(std::llround(std::max(0.0, seconds) * rate));
    return time::RationalTime{frames, base.rate()};
}

time::RationalTime Clip::baseSourceTimeAt(const time::RationalTime& timelineTime) const {
    return baseSourceTimeAt(timelineTime, sourceRange.start().rate());
}

time::RationalTime Clip::baseSourceTimeAt(const time::RationalTime& timelineTime,
                                          const time::Rational& atRate) const {
    ZARO_CHECK(!timelineRange.isEmpty(), "sourceTimeAt on a zero-length clip");

    const time::RationalTime offset = timelineTime - timelineRange.start();
    if (!reversed && timelineRange.duration() == sourceRange.duration()) {
        // The common case: same rate, normal speed, forwards. Stay in integer
        // frames rather than round-tripping through a ratio that can only lose.
        //
        // Rescaled to the rate the caller asked for rather than to the source's
        // own: at the source rate this truncates, and for a caller reading in
        // audio blocks the truncation is most of the signal. See the header.
        return sourceRange.start().rescaledTo(atRate) + offset.rescaledTo(atRate);
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
    return sourceRange.start().rescaledTo(atRate) + time::RationalTime::fromSeconds(into, atRate);
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

double Clip::animationSecondsAt(const time::RationalTime& timelineTime) const {
    const double seconds = sourceSecondsAt(timelineTime);
    if (!responsive.isSet()) {
        return seconds;
    }
    const double authored = responsive.authored.toSecondsDouble();
    const double length = sourceRange.duration().toSecondsDouble();
    const double intro = responsive.intro.toSecondsDouble();
    const double outro = responsive.outro.toSecondsDouble();
    if (length <= 0.0 || authored <= 0.0) {
        return seconds;
    }
    const double start = sourceRange.start().toSecondsDouble();
    const double into = seconds - start;

    // Too short to hold both ends: they are scaled down together rather than
    // one of them winning. A title trimmed to less than its own animation is
    // somebody asking for a faster animation, and dropping the exit entirely
    // is not a faster animation, it is a missing one.
    double head = intro;
    double tail = outro;
    if (head + tail > length) {
        const double squeeze = length / (head + tail);
        head *= squeeze;
        tail *= squeeze;
    }

    if (into <= head) {
        // Glued to the start: the intro runs at the speed it was drawn at.
        return start + (into * (intro > 0.0 ? intro / std::max(head, 1e-9) : 1.0));
    }
    if (into >= length - tail) {
        // Glued to the end, measured back from it, in the authored animation.
        const double back = (length - into) * (outro > 0.0 ? outro / std::max(tail, 1e-9) : 1.0);
        return start + authored - back;
    }
    // The middle, stretched to fill what is left between them.
    const double middle = length - head - tail;
    const double authoredMiddle = authored - intro - outro;
    if (middle <= 0.0 || authoredMiddle <= 0.0) {
        return start + intro;
    }
    return start + intro + (((into - head) / middle) * authoredMiddle);
}

Transform Clip::transformAt(const time::RationalTime& timelineTime) const {
    if (animation.empty()) {
        return transform;
    }
    const double seconds = animationSecondsAt(timelineTime);
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

    // The stabiliser's answer, added on top rather than written into the
    // curves above: whatever somebody framed or animated stays theirs, and
    // clearing the analysis restores it exactly.
    animated.positionX += Curve::valueOr(animation.find(Param::StabiliseX), seconds, 0.0);
    animated.positionY += Curve::valueOr(animation.find(Param::StabiliseY), seconds, 0.0);
    const double zoom = Curve::valueOr(animation.find(Param::StabiliseZoom), seconds, 1.0);
    animated.scaleX *= zoom;
    animated.scaleY *= zoom;
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
        case Param::StabiliseZoom:
            // One, not zero: this one multiplies. A stabilise zoom of zero
            // would collapse the picture to nothing, which is not what "no
            // stabilisation" means.
            return 1.0;
        case Param::TextReveal:
            // All of it, for the same shape of reason: a title that is not
            // being typed on is a title you can read.
            return 1.0;
        case Param::StabiliseX:
        case Param::StabiliseY:
        case Param::MaskX:
        case Param::MaskY:
            // No static value either: the mask already records where it is,
            // and a second place for that would be a second thing to keep
            // agreeing with the first. These curves say how far it has been
            // moved from there, which is nothing until something moves it.
            return 0.0;
        case Param::TimeRemap:
            // No static value, on purpose. Every other parameter has a value
            // the clip holds when nothing is animated; a time remap that is not
            // animated is not a remap at all, it is the clip's ordinary
            // mapping. Giving it a static value would mean a "remap of 0
            // seconds" -- a clip frozen on its first frame -- was one stopwatch
            // click away from every clip in the project.
            return 0.0;
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
        case Param::MaskX:
        case Param::MaskY:
        case Param::StabiliseX:
        case Param::StabiliseY:
        case Param::StabiliseZoom:
        case Param::TextReveal:
        case Param::TimeRemap:
            // Nothing to set: see parameterValue.
            return;
    }
}

double Clip::parameterAt(Param param, const time::RationalTime& timelineTime) const {
    const Curve* curve = animation.find(param);
    if (curve == nullptr || curve->empty()) {
        return parameterValue(param);
    }
    return curve->valueAtSeconds(animationSecondsAt(timelineTime));
}

ColorCorrection Clip::colorAt(const time::RationalTime& timelineTime) const {
    if (animation.empty()) {
        return color;
    }
    const double seconds = animationSecondsAt(timelineTime);
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

Mask Clip::maskAt(const time::RationalTime& timelineTime) const {
    if (!mask.isSet() || animation.empty()) {
        return mask;
    }
    const double seconds = animationSecondsAt(timelineTime);
    const double dx = Curve::valueOr(animation.find(Param::MaskX), seconds, 0.0);
    const double dy = Curve::valueOr(animation.find(Param::MaskY), seconds, 0.0);
    if (dx == 0.0 && dy == 0.0) {
        return mask;
    }
    Mask moved = mask;
    moved.centreX += dx;
    moved.centreY += dy;
    for (MaskPoint& point : moved.path.points) {
        // The point only: the handles are offsets from it, so moving them as
        // well would move them twice and open the curve out as it travelled.
        point.x += dx;
        point.y += dy;
    }
    return moved;
}

double Clip::gainDbAt(const time::RationalTime& timelineTime) const {
    if (animation.empty()) {
        return gainDb;
    }
    return Curve::valueOr(animation.find(Param::GainDb), animationSecondsAt(timelineTime), gainDb);
}

double Clip::panAt(const time::RationalTime& timelineTime) const {
    if (animation.empty()) {
        return pan;
    }
    return Curve::valueOr(animation.find(Param::Pan), animationSecondsAt(timelineTime), pan);
}

}  // namespace zaro::model
