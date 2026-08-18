#pragma once

#include <vector>

#include "zaro/core/model/Sequence.h"
#include "zaro/core/time/RationalTime.h"

namespace zaro::edit {

/// What a snapped time latched onto, so the UI can draw the indicator.
enum class SnapKind { None, SequenceStart, ClipStart, ClipEnd, Playhead };

struct SnapResult {
    time::RationalTime time;
    SnapKind kind{SnapKind::None};
    model::TrackId track;

    [[nodiscard]] bool snapped() const noexcept { return kind != SnapKind::None; }
};

/// Pull `t` to the nearest edit point within `threshold`.
///
/// Threshold is a duration rather than a pixel distance because this is core
/// logic and knows nothing about zoom. The UI converts: a fixed pixel radius
/// becomes a shrinking time radius as you zoom in, which is what makes snapping
/// feel helpful rather than obstructive.
///
/// Ties go to the earlier candidate, so the result does not flicker between two
/// equidistant edges as the pointer moves.
[[nodiscard]] SnapResult snapTime(const model::Sequence& sequence, const time::RationalTime& t,
                                  const time::RationalTime& threshold, model::ClipId ignoring = {},
                                  const time::RationalTime* playhead = nullptr);

/// Every point snapping would consider, in ascending order. Exposed for the
/// timeline to draw, and for tests.
[[nodiscard]] std::vector<time::RationalTime> snapCandidates(const model::Sequence& sequence,
                                                             model::ClipId ignoring = {});

}  // namespace zaro::edit
