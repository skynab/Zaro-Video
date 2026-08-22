#pragma once

#include <cstdint>
#include <string>

#include "zaro/core/model/Ids.h"
#include "zaro/core/time/TimeRange.h"

namespace zaro::model {

/// A note on the timeline.
///
/// Markers carry a duration rather than being points, because most of what
/// people mark is a span — a section, a problem, a bit to revisit — and a point
/// is just a span of one frame. Modelling the point case only would mean
/// discovering that later and migrating every project file.
struct Marker {
    MarkerId id;
    time::TimeRange range;
    std::string name;
    std::string note;
    /// An index into whatever palette the UI shows, rather than a colour value:
    /// the meaning is "the green one", and the palette can change.
    std::int32_t colour{0};

    /// Who left this note, when it came from a review rather than from the
    /// person cutting. Empty for an ordinary marker, which is what most of
    /// them are.
    std::string author;

    /// Whether it has been dealt with.
    ///
    /// Kept rather than deleted, because "what did they ask for and what did
    /// we do" is the question a review is for, and a note that vanished when
    /// it was actioned takes half the answer with it.
    bool resolved{false};

    /// Whether this reads as a review comment rather than a working marker:
    /// somebody's name is on it, or a note is.
    [[nodiscard]] bool isComment() const { return !author.empty() || !note.empty(); }

    [[nodiscard]] bool isPoint() const { return range.duration().frames() <= 1; }

    friend bool operator==(const Marker&, const Marker&) = default;
};

}  // namespace zaro::model
