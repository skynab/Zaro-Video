#pragma once

#include <cstdint>

#include "zaro/core/model/Ids.h"
#include "zaro/core/time/TimeRange.h"

namespace zaro::model {

enum class TransitionKind : std::uint8_t {
    CrossDissolve,
    /// The incoming shot is revealed behind a moving edge. Both shots stay
    /// where they are; what moves is the boundary.
    Wipe,
    /// The incoming shot travels in from off screen and pushes nothing: the
    /// outgoing one stays put underneath.
    Slide,
};

/// Which way a wipe's edge, or a slide's picture, travels.
///
/// The same word for both on purpose: a wipe to the right uncovers from the
/// left, and a slide to the right enters from the left, so a person who has
/// chosen a direction for one already knows what it does for the other.
enum class TransitionDirection : std::uint8_t {
    Right,
    Left,
    Down,
    Up,
};

[[nodiscard]] const char* toString(TransitionKind kind) noexcept;
[[nodiscard]] TransitionKind transitionKindFromString(const char* name) noexcept;
[[nodiscard]] const char* toString(TransitionDirection direction) noexcept;
[[nodiscard]] bool transitionDirectionFromString(const char* name,
                                                 TransitionDirection& out) noexcept;

/// A blend across a cut.
///
/// The two clips stay adjacent and do not overlap on the timeline: a transition
/// is not two clips laid on top of each other, it is a span *straddling* the
/// cut during which both are shown. Keeping the clips non-overlapping means
/// every invariant the track already enforces still holds, and it is what lets
/// a transition be removed without having to work out where the clips should
/// go afterwards.
///
/// During the span the outgoing clip is asked for frames past its out point and
/// the incoming clip for frames before its in point. Those frames come from the
/// media either side of the cut -- the handles -- which is why a transition
/// cannot be added where there is no material to reach into.
struct Transition {
    TransitionId id;
    /// Outgoing. Invalid means there is nothing on the way out: the span is a
    /// fade *in*, from black or from silence.
    ClipId from;
    /// Incoming. Invalid means there is nothing on the way in: the span is a
    /// fade *out*, to black or to silence.
    ///
    /// A one-sided span is not a special case bolted on; it is the same idea
    /// with one side empty, which is why it is spelled as a missing clip
    /// rather than as a separate kind. It also behaves differently in one
    /// respect worth knowing: a two-sided span straddles a cut and reads both
    /// clips into their handles, while a one-sided one lies *inside* its clip
    /// and reads no handles at all -- so a fade out can always be added, even
    /// to a clip that uses every frame of its source.
    ClipId to;
    /// On the timeline, straddling the cut between the two clips.
    time::TimeRange range;
    TransitionKind kind{TransitionKind::CrossDissolve};
    /// Ignored by a cross dissolve, which has no direction to travel in.
    TransitionDirection direction{TransitionDirection::Right};

    /// How far through, from 0 at the start to 1 at the end.
    [[nodiscard]] double progressAt(const time::RationalTime& t) const;

    /// Whether this span joins two clips rather than fading one against
    /// nothing.
    [[nodiscard]] bool isCrossFade() const noexcept { return from.isValid() && to.isValid(); }
    /// A fade up from black or silence.
    [[nodiscard]] bool isFadeIn() const noexcept { return !from.isValid() && to.isValid(); }
    /// A fade down to black or silence.
    [[nodiscard]] bool isFadeOut() const noexcept { return from.isValid() && !to.isValid(); }

    friend bool operator==(const Transition&, const Transition&) = default;
};

}  // namespace zaro::model
