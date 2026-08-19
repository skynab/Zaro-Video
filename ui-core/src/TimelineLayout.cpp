#include "zaro/ui/TimelineLayout.h"

#include <array>
#include <cmath>

namespace zaro::ui {
namespace {

/// Zoom bounds. The lower one keeps a four-hour sequence from collapsing into
/// nothing; the upper one stops a single frame filling the screen, past which
/// scrolling becomes useless.
constexpr double kMinPixelsPerSecond = 0.05;
constexpr double kMaxPixelsPerSecond = 40000.0;

/// The ruler ladder, in seconds. A step is chosen by finding the first one wide
/// enough that its labels do not collide.
constexpr std::array<double, 16> kStepSeconds{
    1.0 / 60.0, 1.0 / 24.0, 0.1,  0.2,  0.5,   1.0,   2.0,   5.0,
    10.0,       15.0,       30.0, 60.0, 120.0, 300.0, 600.0, 1800.0,
};

constexpr double kMinimumStepPixels = 90.0;

}  // namespace

void TimelineLayout::setScroll(const time::RationalTime& start) {
    // Never negative. Scrolling before zero shows nothing useful and makes
    // every derived coordinate signed for no benefit.
    scroll_ = start.frames() < 0 ? time::RationalTime{0, start.rate()} : start;
}

void TimelineLayout::setViewportSize(std::int32_t width, std::int32_t height) {
    viewportWidth_ = std::max(0, width);
    viewportHeight_ = std::max(0, height);
}

double TimelineLayout::xForTime(const time::RationalTime& t) const {
    const double seconds = (t - scroll_).toSecondsDouble();
    return metrics_.headerWidth + seconds * metrics_.pixelsPerSecond;
}

time::RationalTime TimelineLayout::timeForX(double x, const time::Rational& frameRate) const {
    const double seconds = (x - metrics_.headerWidth) / metrics_.pixelsPerSecond;

    // Floor, not round. A pixel sits *inside* a frame for that frame's whole
    // width; rounding to nearest names the next frame from halfway across, so a
    // click near the right edge of a clip resolves past its exclusive end and
    // hits nothing. The playback clock has exactly the same requirement, for
    // exactly the same reason.
    const std::int64_t frames = (time::Rational::approximate(seconds) * frameRate).floorToInt();
    const time::RationalTime result =
        scroll_.rescaledTo(frameRate) + time::RationalTime{frames, frameRate};
    return result.frames() < 0 ? time::RationalTime{0, frameRate} : result;
}

time::TimeRange TimelineLayout::visibleRange(const time::Rational& frameRate) const {
    const time::RationalTime start = scroll_.rescaledTo(frameRate);
    const double seconds = contentWidth() / metrics_.pixelsPerSecond;

    // Ceiling, because this is a culling range: a frame only half on screen
    // still has to be painted, and under-covering by half a frame leaves a
    // sliver of the timeline blank at the right edge.
    std::int64_t frames = (time::Rational::approximate(seconds) * frameRate).ceilToInt();
    // Always at least one frame, so a zero-width viewport still produces a
    // range that callers can reason about rather than an empty one.
    frames = std::max<std::int64_t>(1, frames);
    const time::RationalTime duration{frames, frameRate};
    return time::TimeRange{start, duration};
}

void TimelineLayout::zoomBy(double factor, double anchorX, const time::Rational& frameRate) {
    if (factor <= 0.0) {
        return;
    }
    // Whatever is under the pointer stays under the pointer. Without this,
    // zooming walks the timeline sideways and every adjustment needs a
    // corrective scroll.
    const time::RationalTime anchorTime = timeForX(anchorX, frameRate);

    const double wanted = metrics_.pixelsPerSecond * factor;
    metrics_.pixelsPerSecond = std::clamp(wanted, kMinPixelsPerSecond, kMaxPixelsPerSecond);

    const double anchorOffsetSeconds = (anchorX - metrics_.headerWidth) / metrics_.pixelsPerSecond;
    const time::RationalTime newScroll =
        anchorTime - time::RationalTime::fromSeconds(
                         time::Rational::approximate(anchorOffsetSeconds), frameRate);
    setScroll(newScroll);
}

void TimelineLayout::zoomToFit(const time::RationalTime& duration) {
    const double seconds = duration.toSecondsDouble();
    if (seconds <= 0.0 || contentWidth() <= 0) {
        return;
    }
    // A little air at the end, so the last clip does not sit flush against the
    // edge of the panel.
    metrics_.pixelsPerSecond =
        std::clamp(contentWidth() / (seconds * 1.04), kMinPixelsPerSecond, kMaxPixelsPerSecond);
    scroll_ = time::RationalTime{0, duration.rate()};
}

std::vector<TimelineLayout::Row> TimelineLayout::rows(const model::Sequence& sequence) const {
    std::vector<Row> out;
    const auto& videoTracks = sequence.videoTracks();
    const auto& audioTracks = sequence.audioTracks();
    out.reserve(videoTracks.size() + audioTracks.size());

    const auto videoCount = static_cast<std::int32_t>(videoTracks.size());
    const std::int32_t videoStride = metrics_.videoTrackHeight + metrics_.trackGap;
    const std::int32_t audioStride = metrics_.audioTrackHeight + metrics_.trackGap;

    // Video stacks upward: V1 sits at the bottom of the video block, directly
    // above A1. Higher video tracks composite over lower ones and are drawn
    // above them, which is the arrangement every editor expects.
    for (std::int32_t i = 0; i < videoCount; ++i) {
        Row row;
        row.track = videoTracks[static_cast<std::size_t>(i)].id();
        row.kind = model::TrackKind::Video;
        row.index = i;
        row.height = metrics_.videoTrackHeight;
        row.top = metrics_.rulerHeight + (videoCount - 1 - i) * videoStride;
        out.push_back(row);
    }

    const std::int32_t audioTop = metrics_.rulerHeight + videoCount * videoStride;
    for (std::size_t i = 0; i < audioTracks.size(); ++i) {
        Row row;
        row.track = audioTracks[i].id();
        row.kind = model::TrackKind::Audio;
        row.index = static_cast<std::int32_t>(i);
        row.height = metrics_.audioTrackHeight;
        row.top = audioTop + static_cast<std::int32_t>(i) * audioStride;
        out.push_back(row);
    }
    return out;
}

std::optional<TimelineLayout::Row> TimelineLayout::rowAt(const model::Sequence& sequence,
                                                         std::int32_t y) const {
    for (const Row& row : rows(sequence)) {
        if (y >= row.top && y < row.top + row.height) {
            return row;
        }
    }
    return std::nullopt;
}

std::int32_t TimelineLayout::contentHeight(const model::Sequence& sequence) const {
    const auto videoCount = static_cast<std::int32_t>(sequence.videoTracks().size());
    const auto audioCount = static_cast<std::int32_t>(sequence.audioTracks().size());
    return metrics_.rulerHeight + videoCount * (metrics_.videoTrackHeight + metrics_.trackGap) +
           audioCount * (metrics_.audioTrackHeight + metrics_.trackGap);
}

bool TimelineLayout::isInHeaders(std::int32_t x) const {
    return x < metrics_.headerWidth;
}

bool TimelineLayout::isInRuler(std::int32_t x, std::int32_t y) const {
    return !isInHeaders(x) && y >= 0 && y < metrics_.rulerHeight;
}

std::optional<TimelineLayout::Hit> TimelineLayout::hitTest(const model::Sequence& sequence,
                                                           std::int32_t x, std::int32_t y) const {
    if (isInHeaders(x) || isInRuler(x, y)) {
        return std::nullopt;
    }
    const auto row = rowAt(sequence, y);
    if (!row) {
        return std::nullopt;
    }
    const model::Track* track = sequence.findTrack(row->track);
    if (track == nullptr) {
        return std::nullopt;
    }

    const model::Clip* clip = track->clipAt(timeForX(x, sequence.frameRate()));
    if (clip == nullptr) {
        return std::nullopt;
    }

    Hit hit;
    hit.track = row->track;
    hit.clip = clip->id;

    // Edges win over the body, and are measured in pixels rather than in time:
    // a trim handle has to stay the same size to the hand however far the
    // timeline is zoomed in.
    const double startX = xForTime(clip->start());
    const double endX = xForTime(clip->endExclusive());
    const double tolerance = metrics_.edgeGrabPixels;

    // The two grab zones must never meet. On a clip only a few pixels wide they
    // otherwise cover the whole thing, and it becomes impossible to select or
    // drag -- every click is a trim. Capping each zone at a third of the width
    // guarantees the middle third is always body.
    const double width = endX - startX;
    const double effective = std::min(tolerance, std::max(0.0, (width - 1.0) / 3.0));

    if (x - startX <= effective) {
        hit.part = Part::InEdge;
    } else if (endX - x <= effective) {
        hit.part = Part::OutEdge;
    } else {
        hit.part = Part::Body;
    }
    return hit;
}

time::RationalTime TimelineLayout::rulerStep(const time::Rational& frameRate) const {
    const double frameSeconds = frameRate.isPositive() ? 1.0 / frameRate.toDouble() : 1.0 / 25.0;

    // A single frame, when there is room for it. At that zoom the ruler is
    // counting frames and anything coarser is unhelpful.
    if (frameSeconds * metrics_.pixelsPerSecond >= kMinimumStepPixels) {
        return time::RationalTime{1, frameRate};
    }
    for (const double step : kStepSeconds) {
        if (step >= frameSeconds && step * metrics_.pixelsPerSecond >= kMinimumStepPixels) {
            return time::RationalTime::fromSeconds(time::Rational::approximate(step), frameRate);
        }
    }
    return time::RationalTime::fromSeconds(time::Rational::approximate(kStepSeconds.back()),
                                           frameRate);
}

}  // namespace zaro::ui
