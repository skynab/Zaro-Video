#include "zaro/core/time/TimeRange.h"

#include <algorithm>
#include <cassert>

namespace zaro::time {

TimeRange::TimeRange(RationalTime start, RationalTime duration)
    : start_{std::move(start)}, duration_{std::move(duration)} {
    assert(duration_.frames() >= 0 && "TimeRange duration must not be negative");
    if (duration_.frames() < 0) {
        duration_ = RationalTime{0, duration_.rate()};
    }
}

TimeRange TimeRange::fromStartEnd(const RationalTime& start, const RationalTime& endExclusive) {
    if (endExclusive <= start) {
        return TimeRange{start, RationalTime{0, start.rate()}};
    }
    return TimeRange{start, endExclusive - start};
}

RationalTime TimeRange::endInclusive() const {
    assert(!isEmpty() && "endInclusive() on an empty range");
    const RationalTime end = endExclusive();
    return RationalTime{end.frames() - 1, end.rate()};
}

bool TimeRange::contains(const RationalTime& t) const {
    return !isEmpty() && t >= start_ && t < endExclusive();
}

bool TimeRange::contains(const TimeRange& other) const {
    if (other.isEmpty()) {
        return contains(other.start_);
    }
    return other.start_ >= start_ && other.endExclusive() <= endExclusive();
}

bool TimeRange::overlaps(const TimeRange& other) const {
    if (isEmpty() || other.isEmpty()) {
        return false;
    }
    return start_ < other.endExclusive() && other.start_ < endExclusive();
}

bool TimeRange::meets(const TimeRange& other) const {
    return start_ <= other.endExclusive() && other.start_ <= endExclusive();
}

std::optional<TimeRange> TimeRange::intersection(const TimeRange& other) const {
    if (!overlaps(other)) {
        return std::nullopt;
    }
    const RationalTime begin = std::max(start_, other.start_);
    const RationalTime end = std::min(endExclusive(), other.endExclusive());
    return TimeRange::fromStartEnd(begin, end);
}

TimeRange TimeRange::extendedBy(const TimeRange& other) const {
    if (other.isEmpty()) {
        return extendedBy(other.start_);
    }
    if (isEmpty()) {
        return other.extendedBy(start_);
    }
    const RationalTime begin = std::min(start_, other.start_);
    const RationalTime end = std::max(endExclusive(), other.endExclusive());
    return TimeRange::fromStartEnd(begin, end);
}

TimeRange TimeRange::extendedBy(const RationalTime& t) const {
    if (isEmpty()) {
        const RationalTime begin = std::min(start_, t);
        const RationalTime end = std::max(start_, t);
        return TimeRange::fromStartEnd(begin, end);
    }
    const RationalTime begin = std::min(start_, t);
    const RationalTime end = std::max(endExclusive(), t + RationalTime{1, t.rate()});
    return TimeRange::fromStartEnd(begin, end);
}

RationalTime TimeRange::clamp(const RationalTime& t) const {
    assert(!isEmpty() && "clamp() against an empty range");
    if (t < start_) {
        return start_;
    }
    const RationalTime last = endInclusive();
    return t > last ? last : t;
}

TimeRange TimeRange::rescaledTo(const Rational& newRate) const {
    return TimeRange{start_.rescaledTo(newRate), duration_.rescaledTo(newRate)};
}

std::string TimeRange::toString() const {
    return "[" + start_.toString() + ", +" + duration_.toString() + ")";
}

}  // namespace zaro::time
