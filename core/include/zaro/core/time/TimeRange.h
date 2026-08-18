#pragma once

#include <optional>
#include <string>

#include "zaro/core/time/RationalTime.h"

namespace zaro::time {

/// A half-open span of time: [start, start + duration).
///
/// Half-open is not a stylistic choice. It is what makes butt-jointed clips
/// tile a timeline without a gap or an overlap: a clip ending at frame 100 and
/// one starting at frame 100 are adjacent, and every frame belongs to exactly
/// one of them. Closed ranges force an off-by-one at every boundary and are how
/// editors end up with one-frame flashes between cuts.
///
/// Durations are never negative.
class TimeRange {
public:
    constexpr TimeRange() noexcept = default;

    TimeRange(RationalTime start, RationalTime duration);

    /// Build from a start and an *exclusive* end. An end at or before the start
    /// yields an empty range.
    [[nodiscard]] static TimeRange fromStartEnd(const RationalTime& start,
                                                const RationalTime& endExclusive);

    [[nodiscard]] const RationalTime& start() const noexcept { return start_; }
    [[nodiscard]] const RationalTime& duration() const noexcept { return duration_; }

    /// One past the last frame in the range.
    [[nodiscard]] RationalTime endExclusive() const { return start_ + duration_; }

    /// The last frame actually inside the range. Undefined for empty ranges,
    /// which is why callers should prefer endExclusive() unless they are
    /// displaying a timecode to a human.
    [[nodiscard]] RationalTime endInclusive() const;

    [[nodiscard]] bool isEmpty() const noexcept { return duration_.isZero(); }

    [[nodiscard]] bool contains(const RationalTime& t) const;
    [[nodiscard]] bool contains(const TimeRange& other) const;

    /// True when the two ranges share at least one frame. Ranges that merely
    /// touch -- one ending exactly where the next begins -- do not overlap.
    [[nodiscard]] bool overlaps(const TimeRange& other) const;

    /// True when the ranges overlap or abut, i.e. no gap between them.
    [[nodiscard]] bool meets(const TimeRange& other) const;

    [[nodiscard]] std::optional<TimeRange> intersection(const TimeRange& other) const;

    /// Smallest range covering both. Note this spans any gap between them.
    [[nodiscard]] TimeRange extendedBy(const TimeRange& other) const;
    [[nodiscard]] TimeRange extendedBy(const RationalTime& t) const;

    /// The nearest instant inside the range. Clamps to endInclusive(), not
    /// endExclusive(), so the result is always a frame the range owns.
    [[nodiscard]] RationalTime clamp(const RationalTime& t) const;

    [[nodiscard]] TimeRange rescaledTo(const Rational& newRate) const;

    friend bool operator==(const TimeRange& lhs, const TimeRange& rhs) noexcept {
        return lhs.start_ == rhs.start_ && lhs.duration_ == rhs.duration_;
    }

    [[nodiscard]] std::string toString() const;

private:
    RationalTime start_{};
    RationalTime duration_{};
};

}  // namespace zaro::time
