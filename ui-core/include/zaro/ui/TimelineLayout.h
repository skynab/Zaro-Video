#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "zaro/core/model/Sequence.h"
#include "zaro/core/time/RationalTime.h"

namespace zaro::ui {

/// The geometry of a timeline: where a time lands in pixels, where a track
/// lands in rows, and what is under the pointer.
///
/// Deliberately free of any toolkit. A timeline's hard parts are arithmetic --
/// zoom that keeps the right frame under the cursor, hit-testing that
/// distinguishes a clip's body from its edges, culling that keeps painting
/// proportional to what is visible rather than to how long the sequence is --
/// and all of it is testable without a window. The widget on top is then mostly
/// painting.
class TimelineLayout {
public:
    struct Metrics {
        /// Zoom. The one number that everything else is derived from.
        double pixelsPerSecond{120.0};
        std::int32_t videoTrackHeight{62};
        std::int32_t audioTrackHeight{48};
        std::int32_t trackGap{1};
        std::int32_t rulerHeight{26};
        /// Width of the track headers, to the left of the time area.
        std::int32_t headerWidth{150};
        /// How close to a clip edge counts as grabbing the edge rather than the
        /// body. Generous, because a trim handle that needs pixel accuracy is
        /// one nobody uses.
        std::int32_t edgeGrabPixels{6};
    };

    TimelineLayout() = default;
    explicit TimelineLayout(Metrics metrics) : metrics_{metrics} {}

    [[nodiscard]] const Metrics& metrics() const noexcept { return metrics_; }
    void setMetrics(const Metrics& metrics) { metrics_ = metrics; }

    // --- Row heights --------------------------------------------------------

    /// Give one row a height of its own, in place of its kind's default.
    ///
    /// Per track rather than per kind, and kept here rather than in `Metrics`
    /// for that reason: two rows of the same kind may be different heights,
    /// which is what makes room for a tall row on the shot being worked on
    /// while everything else stays out of the way.
    void setTrackHeight(model::TrackId track, std::int32_t height);
    /// Forget one override, or all of them: the rows go back to their kind's
    /// default height.
    void clearTrackHeight(model::TrackId track);
    void clearTrackHeights();
    /// The height this track's row is drawn at -- its own if it has one, and
    /// its kind's otherwise.
    [[nodiscard]] std::int32_t heightOf(model::TrackId track, model::TrackKind kind) const;

    /// Leftmost visible time. Never negative: scrolling before the start of the
    /// sequence shows nothing useful and makes every coordinate signed.
    [[nodiscard]] const time::RationalTime& scroll() const noexcept { return scroll_; }
    void setScroll(const time::RationalTime& start);

    void setViewportSize(std::int32_t width, std::int32_t height);
    [[nodiscard]] std::int32_t viewportWidth() const noexcept { return viewportWidth_; }
    [[nodiscard]] std::int32_t viewportHeight() const noexcept { return viewportHeight_; }

    /// Width of the area that shows time, excluding the headers.
    [[nodiscard]] std::int32_t contentWidth() const noexcept {
        return std::max(0, viewportWidth_ - metrics_.headerWidth);
    }

    // --- Time and pixels ----------------------------------------------------

    /// Widget x for a timeline time, including the header offset.
    [[nodiscard]] double xForTime(const time::RationalTime& t) const;
    /// The time at a widget x. Clamped at zero.
    [[nodiscard]] time::RationalTime timeForX(double x, const time::Rational& frameRate) const;

    /// The frame nearest a widget x, rather than the one it is standing in.
    ///
    /// `timeForX` floors, because a pixel is inside a frame for that frame's
    /// whole width and hit-testing has to agree with that. Aiming is the other
    /// question: a pointer a hair to the left of an edit point is inside the
    /// frame *before* it, and measuring from that frame's start says the hand
    /// is up to a whole frame further away than it is. Anything asking "what is
    /// this pointer near" -- snapping, above all -- wants this one.
    [[nodiscard]] time::RationalTime nearestTimeForX(double x,
                                                     const time::Rational& frameRate) const;

    /// The span currently on screen. What painting and hit-testing iterate
    /// over, so that a four-hour sequence costs the same to draw as a
    /// four-minute one.
    [[nodiscard]] time::TimeRange visibleRange(const time::Rational& frameRate) const;

    /// Zoom by `factor`, keeping whatever time is under `anchorX` in place.
    /// Anchoring on the pointer is what makes zooming feel like the timeline is
    /// being pulled rather than jumping.
    void zoomBy(double factor, double anchorX, const time::Rational& frameRate);

    /// Fit `duration` into the viewport, with a little air at the end.
    void zoomToFit(const time::RationalTime& duration);

    // --- Rows ---------------------------------------------------------------

    struct Row {
        model::TrackId track;
        model::TrackKind kind{model::TrackKind::Video};
        std::int32_t top{0};
        std::int32_t height{0};
        /// Index within its own kind: 0 is V1, or A1.
        std::int32_t index{0};
    };

    /// Video above, audio below, with V1 immediately above A1 -- the
    /// arrangement every editor expects, and the reason video rows are laid out
    /// bottom-up while audio rows are laid out top-down.
    [[nodiscard]] std::vector<Row> rows(const model::Sequence& sequence) const;
    [[nodiscard]] std::optional<Row> rowAt(const model::Sequence& sequence, std::int32_t y) const;

    /// Total height the tracks need, for scrollbar range.
    [[nodiscard]] std::int32_t contentHeight(const model::Sequence& sequence) const;

    // --- Hit testing --------------------------------------------------------

    enum class Part { Body, InEdge, OutEdge };

    struct Hit {
        model::TrackId track;
        model::ClipId clip;
        Part part{Part::Body};
    };

    /// What is under a widget point, if anything. Points over the headers or
    /// the ruler are not hits.
    [[nodiscard]] std::optional<Hit> hitTest(const model::Sequence& sequence, std::int32_t x,
                                             std::int32_t y) const;

    /// Every clip inside a rectangle, for rubber-band selection.
    ///
    /// A clip counts if it overlaps at all rather than only if it is wholly
    /// enclosed: dragging a band across a timeline is a gesture at the scale of
    /// the whole track, and requiring full containment means the long clip you
    /// were obviously pointing at is the one thing left out.
    [[nodiscard]] std::vector<Hit> hitTestRect(const model::Sequence& sequence, std::int32_t x0,
                                               std::int32_t y0, std::int32_t x1,
                                               std::int32_t y1) const;

    // --- Keyframes ----------------------------------------------------------

    /// The lane along the bottom of a clip where keyframes are drawn.
    ///
    /// Along the bottom rather than across the middle: the middle is where the
    /// clip's name and its waveform go, and a diamond on top of a waveform is
    /// unreadable in both directions.
    [[nodiscard]] std::int32_t keyframeLaneHeight() const noexcept;

    struct KeyframeHit {
        model::TrackId track;
        model::ClipId clip;
        /// In the clip's source time, which is where the model keeps it.
        time::RationalTime time;
    };

    /// The keyframe under a point, if the point is in a clip's keyframe lane.
    ///
    /// One hit per *instant*, not per parameter: eight parameters keyed
    /// together are drawn as one diamond, because they are one decision and
    /// because eight stacked diamonds in a lane six pixels tall are one
    /// diamond that cannot be aimed at.
    [[nodiscard]] std::optional<KeyframeHit> hitTestKeyframe(const model::Sequence& sequence,
                                                             std::int32_t x, std::int32_t y) const;

    /// Every instant at which this clip has a keyframe, in source time, sorted.
    [[nodiscard]] static std::vector<time::RationalTime> keyframeTimes(const model::Clip& clip);

    /// Whether a point is in the ruler, where dragging scrubs the playhead.
    [[nodiscard]] bool isInRuler(std::int32_t x, std::int32_t y) const;
    /// Whether a point is over the track headers.
    [[nodiscard]] bool isInHeaders(std::int32_t x) const;

    /// A sensible tick interval for the ruler at the current zoom: the finest
    /// division that still leaves labels readable.
    [[nodiscard]] time::RationalTime rulerStep(const time::Rational& frameRate) const;

private:
    Metrics metrics_{};
    /// Rows given a height of their own. Absent means "whatever the kind says",
    /// so a sequence nobody has resized costs nothing to lay out.
    std::map<model::TrackId, std::int32_t> heights_;
    time::RationalTime scroll_{};
    std::int32_t viewportWidth_{0};
    std::int32_t viewportHeight_{0};
};

}  // namespace zaro::ui
