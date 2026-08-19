#pragma once

#include <cstdint>

#include "zaro/core/model/Ids.h"
#include "zaro/core/time/TimeRange.h"

namespace zaro::model {

enum class TransitionKind : std::uint8_t {
    CrossDissolve,
};

[[nodiscard]] const char* toString(TransitionKind kind) noexcept;
[[nodiscard]] TransitionKind transitionKindFromString(const char* name) noexcept;

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
    ClipId from;  ///< Outgoing.
    ClipId to;    ///< Incoming.
    /// On the timeline, straddling the cut between the two clips.
    time::TimeRange range;
    TransitionKind kind{TransitionKind::CrossDissolve};

    /// How far through, from 0 at the start to 1 at the end.
    [[nodiscard]] double progressAt(const time::RationalTime& t) const;

    friend bool operator==(const Transition&, const Transition&) = default;
};

}  // namespace zaro::model
