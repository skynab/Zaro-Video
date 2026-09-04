#include "TimelineWidget.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/edit/Snapping.h"
#include "zaro/core/model/Graphic.h"
#include "zaro/core/time/Timecode.h"

#include "Icons.h"
#include "Theme.h"
#include "ThumbnailCache.h"
#include "TitlePresets.h"

namespace zaro::app {
namespace {

// Every colour here comes from the design system, through Theme: a timeline
// painted by hand is the one panel Qt's stylesheet cannot reach, so it asks for
// the same tokens the stylesheet was built from rather than keeping a second
// palette that drifts from it.
const QColor kBackground = theme::bg();
const QColor kRulerBackground = theme::mix(theme::surface(), theme::bg(), 0.40);
const QColor kHeaderBackground = theme::mix(theme::surface(), theme::bg(), 0.55);
const QColor kVideoLane = theme::mix(theme::surface(), theme::bg(), 0.46);
// The audio lane carries a trace of its own family, so which half of the
// timeline you are looking at reads before any one clip in it does.
const QColor kAudioLane =
    theme::mix(theme::mix(theme::surface(), theme::bg(), 0.72), theme::audio(600), 0.08);
const QColor kGridLine = theme::textAt(0.14);
const QColor kBand = theme::accent(400);
// The alignment guide and the blade's own line. Both in the accent, because
// both are the interface saying "here", and told apart by how they are drawn
// rather than by colour.
const QColor kSnapGuide = theme::accent(300);
const QColor kBladeLine = theme::accent(200);
const QColor kPlayhead = theme::accent(200);
// One colour, not three: a bar with green, yellow and red in it is three
// claims, and only one of them -- "this will play" -- is one we can make. The
// design spends the accent on it, which is also what keeps the ruler to the two
// hues the rest of the timeline is drawn from.
const QColor kCachedSpan = theme::accent(500);
const QColor kKeyframe = theme::neutral(200);
const QColor kKeyframeHeld = theme::accent(300);
const QColor kKeyframeOutline = theme::bg();
const QColor kDimText = theme::textAt(0.45);
const QColor kTransition = theme::accent(300);
const QColor kTrackFlag = theme::accent(400);
// The V1/A2 badge in a track header, in its own family's colour.
const QColor kVideoTrackId = theme::accent(400);
const QColor kAudioTrackId = theme::audio(400);

// A header control's hit box, and the air around the row of them. Bigger than
// the glyph inside it: thirteen pixels of icon is a target nobody hits, and the
// box is what the pointer is actually aiming at.
constexpr int kControlSize = 17;
constexpr int kControlMargin = 6;
/// Height of the pills in the corner box, and the gap between them.
constexpr int kPillHeight = 19;
constexpr int kPillGap = 5;
/// What a badge is shadowed with when it sits over a picture.
const QColor kBadgeShadow{0, 0, 0, 170};

/// How close to a row's bottom edge counts as grabbing it.
constexpr int kResizeGrabPixels = 4;
/// The range a track height may be dragged to. The floor is what the three
/// header buttons and a legible clip need; the ceiling is where a taller row
/// has stopped telling you anything more.
constexpr int kMinTrackHeight = 26;
constexpr int kMaxTrackHeight = 220;

/// The mark a track carries when it will not shift under a ripple.
const QString kSyncUnlocked = QStringLiteral("\u2226");

/// Marker colours, indexed by the marker's own colour field. The model stores
/// "the green one" rather than a colour value, so the palette can change
/// without rewriting every project file.
const QColor kMarkerPalette[] = {
    QColor{236, 196, 92},  QColor{120, 200, 130}, QColor{110, 170, 235},
    QColor{224, 120, 160}, QColor{200, 150, 235}, QColor{230, 140, 90},
};

/// How a clip is painted: its body, the strip along its top, and its label.
///
/// Three families rather than two. Generated pictures -- titles, shapes, text
/// -- are not footage and reading them as footage is the mistake the strip is
/// there to prevent: a title track that looks like a video track is a track
/// whose clips you go looking for the media of.
///
/// Four things rather than three: the outline a selected clip gets belongs to
/// the family too, because an accent-coloured ring around a teal clip is the
/// one moment the two palettes would touch.
struct ClipPaint {
    QColor body;
    QColor strip;
    QColor label;
    QColor ring;
};

ClipPaint clipPaint(model::TrackKind kind, const model::Clip& clip) {
    if (clip.graphic.kind != model::GraphicKind::None) {
        return {theme::neutral(900), theme::neutral(500), theme::neutral(200), theme::neutral(300)};
    }
    if (kind == model::TrackKind::Audio) {
        return {theme::mix(theme::bg(), theme::audio(900), 0.85), theme::audio(400),
                theme::audio(200), theme::audio(300)};
    }
    return {theme::mix(theme::bg(), theme::accent(800), 0.85), theme::accent(400),
            theme::accent(100), theme::accent(300)};
}

/// The link badge, cached by colour.
///
/// Drawn once per colour rather than once per clip: a repaint of a busy
/// timeline draws this on every clip in view, and rasterising the same nine
/// pixels a hundred times a frame is work nobody asked for. Three colours can
/// ever be asked for -- the three clip families -- so the cache is bounded by
/// construction.
const QPixmap& linkBadge(const QColor& ink) {
    static std::map<QRgb, QPixmap> cache;
    const QRgb key = ink.rgba();
    auto found = cache.find(key);
    if (found == cache.end()) {
        found = cache.emplace(key, icons::pixmap(icons::Glyph::Link, 11, ink)).first;
    }
    return found->second;
}

/// The effects badge, cached the same way and for the same reason.
const QPixmap& effectsBadge(const QColor& ink) {
    static std::map<QRgb, QPixmap> cache;
    const QRgb key = ink.rgba();
    auto found = cache.find(key);
    if (found == cache.end()) {
        found = cache.emplace(key, icons::pixmap(icons::Glyph::Sparkle, 11, ink)).first;
    }
    return found->second;
}

}  // namespace

TimelineWidget::TimelineWidget(QWidget* parent) : QWidget{parent} {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumHeight(180);
    setAutoFillBackground(false);
    // Files dragged out of the media pane land here.
    setAcceptDrops(true);
}

void TimelineWidget::setTool(Tool tool) {
    if (tool_ == tool) {
        return;
    }
    tool_ = tool;
    // Any half-finished gesture belongs to the tool that started it.
    finishDrag();
    clearGestureMarks();
    unsetCursor();
    emit toolChanged();
    update();
}

void TimelineWidget::setSnapEnabled(bool enabled) {
    if (snapEnabled_ == enabled) {
        return;
    }
    snapEnabled_ = enabled;
    emit snapChanged(snapEnabled_);
}

void TimelineWidget::bind(const ui::SequenceBinding& binding) {
    // Whatever was being typed belonged to the old sequence. Abandoned rather
    // than committed, because the track it named may not be in the new one.
    finishRename(false);
    project_ = binding.project;
    sequenceId_ = binding.sequence;
    commands_ = binding.commands;
    selected_ = {};
    // Defer the fit: at this point the widget has not been laid out, so its
    // width is not yet the width it will be shown at, and fitting to it would
    // put the whole sequence in the wrong scale.
    pendingFit_ = true;
    update();
}

const model::Sequence* TimelineWidget::sequence() const {
    return project_ != nullptr ? project_->findSequence(sequenceId_) : nullptr;
}

void TimelineWidget::setPlayhead(const time::RationalTime& position) {
    if (playhead_ == position) {
        return;
    }
    playhead_ = position;
    followPlayhead();
    update();
}

void TimelineWidget::followPlayhead() {
    const model::Sequence* seq = sequence();
    if (seq == nullptr || layout_.contentWidth() <= 0) {
        return;
    }
    const time::Rational& rate = seq->frameRate();
    const time::TimeRange visible = layout_.visibleRange(rate);
    if (visible.contains(playhead_.rescaledTo(rate))) {
        return;
    }

    // Off screen. Page rather than centre: during playback, centring means the
    // timeline scrolls continuously under a stationary playhead, which is far
    // harder to read than a view that jumps once per screenful.
    const std::int64_t span = visible.duration().frames();
    const std::int64_t margin = span / 8;
    const std::int64_t start = playhead_.rescaledTo(rate).frames() - margin;
    layout_.setScroll(time::RationalTime{std::max<std::int64_t>(0, start), rate});
}

int TimelineWidget::trackHeight(model::TrackKind kind) const {
    return kind == model::TrackKind::Audio ? layout_.metrics().audioTrackHeight
                                           : layout_.metrics().videoTrackHeight;
}

/// The height asked for, from the height drawn.
///
/// Everything that sets a height is pointing at the screen -- a pointer on a
/// row's edge, a number in a test -- and what is remembered is the height
/// before the panel-wide scale, so that scaling and resizing compose instead of
/// fighting.
static int unscaled(int pixels, double scale) {
    return static_cast<int>(std::lround(static_cast<double>(pixels) / std::max(0.01, scale)));
}

void TimelineWidget::setTrackHeight(model::TrackKind kind, int pixels) {
    // What was asked for is remembered separately from what fits. A panel
    // dragged short squashes the rows, and dragging it tall again should give
    // back the height that was chosen -- which it cannot do if the squashed
    // value has overwritten it.
    (kind == model::TrackKind::Audio ? wantedAudioHeight_ : wantedVideoHeight_) =
        std::clamp(unscaled(pixels, heightScale_), kMinTrackHeight, kMaxTrackHeight);

    // Setting the height of a kind means all of its rows, so the ones that had
    // a height of their own give it up rather than silently ignoring the
    // instruction.
    const model::Sequence* seq = sequence();
    if (seq != nullptr) {
        for (const model::Track& track :
             kind == model::TrackKind::Audio ? seq->audioTracks() : seq->videoTracks()) {
            wantedHeights_.erase(track.id());
        }
    }
    applyTrackHeights();
}

int TimelineWidget::trackHeight(model::TrackId track) const {
    const model::Sequence* seq = sequence();
    const model::Track* found = seq != nullptr ? seq->findTrack(track) : nullptr;
    if (found == nullptr) {
        return 0;
    }
    return layout_.heightOf(track, found->kind());
}

void TimelineWidget::setTrackHeight(model::TrackId track, int pixels) {
    if (!track.isValid()) {
        return;
    }
    wantedHeights_[track] =
        std::clamp(unscaled(pixels, heightScale_), kMinTrackHeight, kMaxTrackHeight);
    applyTrackHeights();
}

void TimelineWidget::clearTrackHeight(model::TrackId track) {
    if (wantedHeights_.erase(track) > 0) {
        applyTrackHeights();
    }
}

int TimelineWidget::wantedHeightFor(model::TrackId track, model::TrackKind kind) const {
    const auto found = wantedHeights_.find(track);
    if (found != wantedHeights_.end()) {
        return found->second;
    }
    return kind == model::TrackKind::Audio ? wantedAudioHeight_ : wantedVideoHeight_;
}

void TimelineWidget::setTrackHeightScale(double scale) {
    const double clamped = std::clamp(scale, kMinTrackHeightScale, kMaxTrackHeightScale);
    if (std::abs(clamped - heightScale_) < 0.0001) {
        return;
    }
    heightScale_ = clamped;
    applyTrackHeights();
    emit viewChanged();
}

double TimelineWidget::trackHeightFraction() const {
    return (heightScale_ - kMinTrackHeightScale) / (kMaxTrackHeightScale - kMinTrackHeightScale);
}

void TimelineWidget::setTrackHeightFraction(double fraction) {
    const double f = std::clamp(fraction, 0.0, 1.0);
    setTrackHeightScale(kMinTrackHeightScale + f * (kMaxTrackHeightScale - kMinTrackHeightScale));
}

void TimelineWidget::applyTrackHeights() {
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    ui::TimelineLayout::Metrics metrics = layout_.metrics();

    const auto scaled = [this](int wanted) {
        return std::clamp(static_cast<int>(std::lround(wanted * heightScale_)), kMinTrackHeight,
                          kMaxTrackHeight);
    };

    // What every row would take if it got what it asked for.
    std::map<model::TrackId, int> heights;
    int total = metrics.rulerHeight;
    for (const auto* tracks : {&seq->videoTracks(), &seq->audioTracks()}) {
        for (const model::Track& track : *tracks) {
            const int height = scaled(wantedHeightFor(track.id(), track.kind()));
            heights[track.id()] = height;
            total += height + metrics.trackGap;
        }
    }

    // Rows do not scroll vertically, so what will not fit is shared out evenly
    // and then floored: when the panel is too small even for the minimum, there
    // is nothing better to do than be as compact as possible.
    int squash = 0;
    if (const int over = total - height(); over > 0 && !heights.empty()) {
        squash = (over + static_cast<int>(heights.size()) - 1) / static_cast<int>(heights.size());
        for (auto& [id, value] : heights) {
            value = std::max(kMinTrackHeight, value - squash);
        }
    }

    // The kind heights follow the same arithmetic. They are what a row that
    // does not exist yet is drawn at -- the strip the drop preview paints for a
    // track it is about to make -- and what `trackHeight(kind)` answers.
    metrics.videoTrackHeight = std::max(kMinTrackHeight, scaled(wantedVideoHeight_) - squash);
    metrics.audioTrackHeight = std::max(kMinTrackHeight, scaled(wantedAudioHeight_) - squash);

    const bool sameMetrics = metrics.videoTrackHeight == layout_.metrics().videoTrackHeight &&
                             metrics.audioTrackHeight == layout_.metrics().audioTrackHeight;
    const bool sameRows =
        std::all_of(heights.begin(), heights.end(), [this, seq](const auto& entry) {
            const model::Track* track = seq->findTrack(entry.first);
            return track != nullptr && layout_.heightOf(entry.first, track->kind()) == entry.second;
        });
    if (sameMetrics && sameRows) {
        return;
    }

    layout_.setMetrics(metrics);
    // Set outright rather than merged: a track removed since the last pass has
    // no row to give a height to, and an override left behind for it would be
    // handed to whatever id came next.
    layout_.clearTrackHeights();
    for (const auto& [id, value] : heights) {
        layout_.setTrackHeight(id, value);
    }
    emit viewChanged();
    update();
}

void TimelineWidget::zoomToFit() {
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    layout_.setViewportSize(width(), height());
    const time::RationalTime duration = seq->duration();
    if (duration.frames() > 0) {
        layout_.zoomToFit(duration);
    }
    discardQueuedThumbnails();
    emit viewChanged();
    update();
}

void TimelineWidget::zoomBy(double factor) {
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    layout_.zoomBy(factor, width() / 2.0, seq->frameRate());
    discardQueuedThumbnails();
    emit viewChanged();
    update();
}

namespace {
// The span a zoom control covers, in pixels per second. Two decades: a
// four-hour assembly at one end, individual frames at the other.
constexpr double kMinPixelsPerSecond = 1.0;
constexpr double kMaxPixelsPerSecond = 600.0;
}  // namespace

double TimelineWidget::zoomFraction() const {
    // Logarithmic, because that is how zoom is felt: the step from 2 to 4
    // pixels a second is the same gesture as the step from 200 to 400.
    const double span = std::log(kMaxPixelsPerSecond / kMinPixelsPerSecond);
    const double at = std::log(
        std::clamp(layout_.metrics().pixelsPerSecond, kMinPixelsPerSecond, kMaxPixelsPerSecond) /
        kMinPixelsPerSecond);
    return std::clamp(at / span, 0.0, 1.0);
}

void TimelineWidget::setZoomFraction(double fraction) {
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    const double span = std::log(kMaxPixelsPerSecond / kMinPixelsPerSecond);
    const double wanted = kMinPixelsPerSecond * std::exp(std::clamp(fraction, 0.0, 1.0) * span);
    const double current = layout_.metrics().pixelsPerSecond;
    if (current <= 0.0) {
        return;
    }
    // Through zoomBy rather than by setting the metric, so the frame under the
    // middle of the view stays where it is.
    layout_.zoomBy(wanted / current, width() / 2.0, seq->frameRate());
    discardQueuedThumbnails();
    emit viewChanged();
    update();
}

void TimelineWidget::undo() {
    if (project_ == nullptr || commands_ == nullptr) {
        return;
    }
    commands_->undo(*project_);
    selection_.clear();
    announceSelection();
    emit edited();
    update();
}

void TimelineWidget::redo() {
    if (project_ == nullptr || commands_ == nullptr) {
        return;
    }
    commands_->redo(*project_);
    selection_.clear();
    announceSelection();
    emit edited();
    update();
}

void TimelineWidget::selectAll() {
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    selection_.clear();
    for (const auto* list : {&seq->videoTracks(), &seq->audioTracks()}) {
        for (const model::Track& track : *list) {
            for (const model::Clip& clip : track.clips()) {
                selection_.push_back(edit::ClipRef{track.id(), clip.id});
            }
        }
    }
    announceSelection();
    update();
}

std::optional<ui::TimelineLayout::Row> TimelineWidget::rowFor(model::TrackId track) const {
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return std::nullopt;
    }
    for (const ui::TimelineLayout::Row& row : layout_.rows(*seq)) {
        if (row.track == track) {
            return row;
        }
    }
    return std::nullopt;
}

void TimelineWidget::resizeEvent(QResizeEvent* event) {
    layout_.setViewportSize(width(), height());
    // A shorter panel may no longer fit the heights that fitted the tall one.
    // Re-derived from what was asked for, so dragging the splitter down and
    // back up gives those heights back rather than leaving them squashed.
    applyTrackHeights();
    if (pendingFit_) {
        pendingFit_ = false;
        zoomToFit();
    }
    QWidget::resizeEvent(event);
}

// --- Painting ---------------------------------------------------------------

void TimelineWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.fillRect(rect(), kBackground);

    if (sequence() == nullptr) {
        painter.setPen(kDimText);
        painter.drawText(rect(), Qt::AlignCenter, "No sequence");
        return;
    }

    paintTracks(painter);
    paintRuler(painter);
    // After the ruler, which fills the full width and would paint over it.
    paintHeaderCorner(painter);
    // Under the playhead, over the clips: the guide is about where an edit is
    // going, and the playhead is where the picture is. When they coincide --
    // which is the whole point of snapping to it -- the playhead should be the
    // line you see.
    paintDropPreview(painter);
    paintMovePreview(painter);
    paintBladePreview(painter);
    paintSnapGuide(painter);
    paintPlayhead(painter);

    if (drag_ == Drag::Band && !band_.isNull()) {
        painter.setPen(QPen(kBand, 1, Qt::DashLine));
        painter.fillRect(band_, QColor(kBand.red(), kBand.green(), kBand.blue(), 40));
        painter.drawRect(band_.adjusted(0, 0, -1, -1));
    }
}

bool TimelineWidget::isSelected(model::ClipId clip) const {
    return std::any_of(selection_.begin(), selection_.end(),
                       [clip](const edit::ClipRef& ref) { return ref.clip == clip; });
}

void TimelineWidget::selectOnly(const ui::TimelineLayout::Hit& hit) {
    selection_.assign(1, edit::ClipRef{hit.track, hit.clip});
    announceSelection();
}

void TimelineWidget::makePrimary(const ui::TimelineLayout::Hit& hit) {
    const auto found =
        std::find_if(selection_.begin(), selection_.end(),
                     [&hit](const edit::ClipRef& ref) { return ref.clip == hit.clip; });
    if (found == selection_.end()) {
        return;
    }
    if (found != selection_.begin()) {
        std::rotate(selection_.begin(), found, found + 1);
    }
    // Announced even when it was already at the front, because being at the
    // front is not the thing that has to be true: `selected_` is what a drag
    // acts on, and it is set from the front of the set only when something
    // announces. A selection can outlive the announcement that produced it --
    // a clip removed and restored by undo, a sequence rebound underneath it --
    // and then the set says one thing and `selected_` another. Announcing
    // unconditionally is what makes the press authoritative.
    announceSelection();
}

void TimelineWidget::toggleSelected(const ui::TimelineLayout::Hit& hit) {
    const auto found =
        std::find_if(selection_.begin(), selection_.end(),
                     [&hit](const edit::ClipRef& ref) { return ref.clip == hit.clip; });
    if (found != selection_.end()) {
        selection_.erase(found);
    } else {
        selection_.push_back(edit::ClipRef{hit.track, hit.clip});
    }
    announceSelection();
}

void TimelineWidget::announceSelection() {
    // The primary selection is the first entry, which is what the parameter
    // panel shows. With nothing selected it reports an invalid id and the panel
    // empties itself.
    selected_ = selection_.empty() ? model::ClipId{} : selection_.front().clip;
    selectedTrack_ = selection_.empty() ? model::TrackId{} : selection_.front().track;
    selectedLink_ = {};
    if (const model::Sequence* seq = sequence(); seq != nullptr && selected_.isValid()) {
        if (const model::Track* track = seq->findTrack(selectedTrack_)) {
            if (const model::Clip* clip = track->find(selected_)) {
                selectedLink_ = clip->link;
            }
        }
    }
    // Picking clips turns a track selection off. Announced even when nothing
    // is selected, because "no clips and no track" is the state a panel has to
    // be able to reach.
    if (!selection_.empty() && headSelected_.isValid()) {
        headSelected_ = {};
        emit trackSelected(headSelected_);
    }
    emit selectionChanged(selectedTrack_, selected_);
    emit selectionSetChanged(selection_);
    update();
}

void TimelineWidget::paintRuler(QPainter& painter) {
    const model::Sequence& seq = *sequence();
    const auto& metrics = layout_.metrics();
    const QRect ruler(0, 0, width(), metrics.rulerHeight);
    painter.fillRect(ruler, kRulerBackground);

    const time::Rational& rate = seq.frameRate();
    const time::TimeRange visible = layout_.visibleRange(rate);
    const time::RationalTime step = layout_.rulerStep(rate);
    if (step.frames() <= 0) {
        return;
    }

    // Start on a multiple of the step, so labels do not crawl as you scroll.
    const std::int64_t first = (visible.start().frames() / step.frames()) * step.frames();
    const bool dropFrame = time::supportsDropFrame(rate);
    const bool withHours = seq.duration().toSecondsDouble() >= 3600.0;

    QFont font = painter.font();
    font.setPointSizeF(9.5);
    painter.setFont(font);
    const QFontMetrics fontMetrics(font);

    for (std::int64_t frame = first; frame <= visible.endExclusive().frames();
         frame += step.frames()) {
        const double x = layout_.xForTime(time::RationalTime{frame, rate});
        if (x < metrics.headerWidth - 1) {
            continue;
        }
        painter.setPen(kGridLine);
        painter.drawLine(QPointF(x, metrics.rulerHeight - 6), QPointF(x, metrics.rulerHeight));

        // Minutes and seconds, not the full timecode. The ruler is for reading
        // position at a glance; the frame-accurate answer is in the status
        // line, and four fields at this size is a smear rather than a number.
        // Hours come back only once the cut is long enough to need them --
        // without that, a two-hour sequence labels 05:00 twice.
        const time::Timecode code = time::timecodeFromFrames(frame, rate, dropFrame);
        const QString label = withHours ? QStringLiteral("%1:%2:%3")
                                              .arg(code.hours)
                                              .arg(code.minutes, 2, 10, QLatin1Char('0'))
                                              .arg(code.seconds, 2, 10, QLatin1Char('0'))
                                        : QStringLiteral("%1:%2")
                                              .arg(code.minutes, 2, 10, QLatin1Char('0'))
                                              .arg(code.seconds, 2, 10, QLatin1Char('0'));
        painter.setPen(kDimText);
        painter.drawText(QPointF(x + 4, fontMetrics.ascent() + 3), label);
    }

    paintCacheBar(painter);

    painter.setPen(kGridLine);
    painter.drawLine(0, metrics.rulerHeight, width(), metrics.rulerHeight);

    paintMarkers(painter);
}

void TimelineWidget::setCachedSpans(std::vector<time::TimeRange> spans) {
    cachedSpans_ = std::move(spans);
    update();
}

/// A thin strip along the bottom of the ruler, showing what is rendered.
///
/// Under the ruler rather than over the tracks: what is cached is a property of
/// the timeline as a whole, not of any one clip, and a stripe across the clips
/// would read as something about them.
void TimelineWidget::paintCacheBar(QPainter& painter) {
    if (cachedSpans_.empty() || sequence() == nullptr) {
        return;
    }
    const auto& metrics = layout_.metrics();
    const double top = metrics.rulerHeight - 3.0;
    painter.setPen(Qt::NoPen);
    painter.setBrush(kCachedSpan);
    for (const time::TimeRange& span : cachedSpans_) {
        const double x0 = std::max<double>(layout_.xForTime(span.start()), metrics.headerWidth);
        const double x1 = layout_.xForTime(span.endExclusive());
        if (x1 <= x0) {
            continue;
        }
        painter.drawRect(QRectF(x0, top, x1 - x0, 3.0));
    }
    painter.setBrush(Qt::NoBrush);
}

void TimelineWidget::paintMarkers(QPainter& painter) {
    const model::Sequence& seq = *sequence();
    const auto& metrics = layout_.metrics();
    const time::TimeRange visible = layout_.visibleRange(seq.frameRate());

    for (const model::Marker& marker : seq.markers()) {
        if (!marker.range.overlaps(visible)) {
            continue;
        }
        const double startX = layout_.xForTime(marker.range.start());
        const double endX = layout_.xForTime(marker.range.endExclusive());
        if (endX < metrics.headerWidth) {
            continue;
        }

        const QColor colour = kMarkerPalette[static_cast<std::size_t>(std::abs(marker.colour)) %
                                             (sizeof(kMarkerPalette) / sizeof(kMarkerPalette[0]))];

        // A spanned marker is drawn as a bar, a point marker as a tab. Drawing
        // both the same way would make a one-frame marker invisible at any zoom
        // where a frame is under a pixel.
        QRectF box(std::max(startX, static_cast<double>(metrics.headerWidth)),
                   metrics.rulerHeight - 7.0, std::max(6.0, endX - startX), 6.0);
        painter.fillRect(box, colour);
        painter.setPen(colour.darker(140));
        painter.drawRect(box.adjusted(0.5, 0.5, -0.5, -0.5));
    }
}

QFont TimelineWidget::badgeFont() const {
    // The mono face the V1/A2 badge is set in. Worked out from the widget's own
    // font so a change of application font carries through.
    QFont badge = font();
    badge.setStyleHint(QFont::Monospace);
    badge.setFamily(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
    badge.setPointSizeF(9.0);
    return badge;
}

QString TimelineWidget::trackBadge(const ui::TimelineLayout::Row& row) {
    return (row.kind == model::TrackKind::Audio ? QStringLiteral("A") : QStringLiteral("V")) +
           QString::number(row.index + 1);
}

QRect TimelineWidget::trackNameRect(const ui::TimelineLayout::Row& row,
                                    const model::Track& track) const {
    // `A9` sizes the badge column, so names line up down the header instead of
    // stepping in and out with the digit.
    const int badgeWidth = QFontMetrics(badgeFont()).horizontalAdvance(QStringLiteral("A9")) + 8;
    // What is left after the buttons on the right and the badge on the left,
    // less the sync-lock mark when that is showing.
    int right = layout_.metrics().headerWidth - (3 * kControlSize + kControlMargin) - 4;
    if (!track.isSyncLocked()) {
        right -= QFontMetrics(font()).horizontalAdvance(kSyncUnlocked) + 4;
    }
    const int left = 10 + badgeWidth;
    return {left, row.top, std::max(0, right - left), row.height};
}

void TimelineWidget::paintTracks(QPainter& painter) {
    const model::Sequence& seq = *sequence();
    const auto& metrics = layout_.metrics();

    const QFont nameFont = painter.font();
    const QFont badges = badgeFont();

    for (const ui::TimelineLayout::Row& row : layout_.rows(seq)) {
        const QRect lane(metrics.headerWidth, row.top, width() - metrics.headerWidth, row.height);
        painter.fillRect(lane, row.kind == model::TrackKind::Audio ? kAudioLane : kVideoLane);
        paintClips(painter, row);

        paintTransitions(painter, row);

        const QRect header(0, row.top, metrics.headerWidth, row.height);
        painter.fillRect(header, kHeaderBackground);
        painter.setPen(kGridLine);
        painter.drawRect(header.adjusted(0, 0, -1, -1));

        const model::Track* track = seq.findTrack(row.track);
        if (track == nullptr) {
            continue;
        }
        // The badge first: V1, A2 -- what the track is called in every
        // conversation about a cut, in the family's colour and in the mono face
        // the rest of the interface uses for anything positional. The name the
        // editor gave the track follows it, because that is the part that
        // changes from project to project.
        const bool isAudio = row.kind == model::TrackKind::Audio;
        const QString badge = trackBadge(row);

        painter.setFont(badges);
        painter.setPen(track->isMuted() ? kDimText : (isAudio ? kAudioTrackId : kVideoTrackId));
        painter.drawText(header.adjusted(10, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, badge);
        // Put the face back before anything else draws. The mono one belongs to
        // the badge and to nothing else, and everything downstream -- the flags
        // below, the clip names on the next row -- derives its font from
        // whatever the painter is currently carrying.
        painter.setFont(nameFont);

        // Sync lock has no button of its own -- the design gives the header
        // three, and this is the one nobody presses mid-edit -- so it takes a
        // sliver of the name's space as a mark, and only when it is off, which
        // is the state worth knowing about.
        if (!track->isSyncLocked()) {
            const int markWidth = QFontMetrics(nameFont).horizontalAdvance(kSyncUnlocked) + 4;
            const QRect nameBox = trackNameRect(row, *track);
            painter.setPen(kTrackFlag);
            painter.drawText(QRect(nameBox.right(), row.top, markWidth, row.height),
                             Qt::AlignVCenter | Qt::AlignRight, kSyncUnlocked);
        }

        // A new sequence names its tracks V1 and A1, and so do most of the
        // formats we import -- so the name is very often the badge again.
        // Drawn only when it says something the badge did not, because "V1 V1"
        // is a header that has used its width to repeat itself. The row being
        // renamed draws nothing here: the editor is sitting on top of it.
        const QString name = QString::fromStdString(track->name());
        if (name.compare(badge, Qt::CaseInsensitive) != 0 && row.track != renamingTrack_) {
            const QRect nameBox = trackNameRect(row, *track);
            painter.setPen(track->isMuted() ? kDimText : theme::textAt(0.72));
            // Elided rather than clipped: a name cut mid-letter reads as a
            // rendering fault, and an ellipsis reads as "there is more".
            painter.drawText(
                nameBox, Qt::AlignVCenter | Qt::AlignLeft,
                QFontMetrics(nameFont).elidedText(name, Qt::ElideRight, nameBox.width()));
        }

        paintTrackControls(painter, row, *track);
    }
}

void TimelineWidget::paintTrackControls(QPainter& painter, const ui::TimelineLayout::Row& row,
                                        const model::Track& track) {
    const bool isAudio = row.kind == model::TrackKind::Audio;
    const bool muted = track.isMuted();
    const bool locked = track.isLocked();

    // Three states, and they are not the same three for each button. Mute and
    // lock are settings, so they read bright when on and quiet when off; remove
    // is an action, so it is quiet until it is pointed at.
    struct Button {
        HeaderControl control;
        icons::Glyph glyph;
        bool on;
    };
    const Button buttons[] = {
        {HeaderControl::Mute,
         isAudio ? (muted ? icons::Glyph::SpeakerSlash : icons::Glyph::SpeakerHigh)
                 : (muted ? icons::Glyph::EyeSlash : icons::Glyph::Eye),
         muted},
        {HeaderControl::Lock, locked ? icons::Glyph::LockClosed : icons::Glyph::LockOpen, locked},
        {HeaderControl::Remove, icons::Glyph::Close, false},
    };

    painter.setRenderHint(QPainter::Antialiasing, true);
    for (const Button& button : buttons) {
        const QRect box = headerControlRect(row, button.control);
        const bool hovered =
            hoverHeader_.track == row.track && hoverHeader_.control == button.control;
        if (hovered) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme::textAt(0.10));
            painter.drawRoundedRect(box, 4.0, 4.0);
            painter.setBrush(Qt::NoBrush);
        }
        // An engaged mute or lock is the track saying something about itself,
        // so it takes the accent. Everything else stays out of the way.
        const QColor ink = button.on ? kTrackFlag : theme::textAt(hovered ? 0.75 : 0.42);
        const QPixmap glyph = icons::pixmap(button.glyph, 13, ink);
        const double scale = glyph.devicePixelRatio();
        painter.drawPixmap(QPointF(box.center().x() - glyph.width() / scale / 2.0 + 0.5,
                                   box.center().y() - glyph.height() / scale / 2.0 + 0.5),
                           glyph);
    }
    painter.setRenderHint(QPainter::Antialiasing, false);
}

void TimelineWidget::paintHeaderCorner(QPainter& painter) {
    const auto& metrics = layout_.metrics();
    const QRect corner(0, 0, metrics.headerWidth, metrics.rulerHeight);
    painter.fillRect(corner, kHeaderBackground);
    painter.setPen(kGridLine);
    painter.drawLine(corner.bottomLeft(), corner.bottomRight());
    painter.drawLine(corner.topRight(), corner.bottomRight());

    QFont pill = painter.font();
    pill.setPointSizeF(8.5);
    painter.setFont(pill);
    painter.setRenderHint(QPainter::Antialiasing, true);

    struct Pill {
        CornerControl control;
        QString label;
        QColor ink;
    };
    const Pill pills[] = {
        {CornerControl::AddVideo, QStringLiteral("Video"), theme::accent(300)},
        {CornerControl::AddAudio, QStringLiteral("Audio"), theme::audio(300)},
    };

    for (const Pill& entry : pills) {
        const QRect box = cornerControlRect(entry.control);
        const bool hovered = hoverCorner_ == entry.control;
        QColor edge = entry.ink;
        edge.setAlphaF(hovered ? 0.75F : 0.38F);
        if (hovered) {
            QColor wash = entry.ink;
            wash.setAlphaF(0.14F);
            painter.setPen(Qt::NoPen);
            painter.setBrush(wash);
            painter.drawRoundedRect(box, 5.0, 5.0);
            painter.setBrush(Qt::NoBrush);
        }
        painter.setPen(QPen(edge, 1.0));
        painter.drawRoundedRect(QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5), 5.0, 5.0);

        const QPixmap plus = icons::pixmap(icons::Glyph::Plus, 9, entry.ink);
        const double scale = plus.devicePixelRatio();
        painter.drawPixmap(
            QPointF(box.left() + 6.0, box.center().y() - plus.height() / scale / 2.0), plus);
        painter.setPen(entry.ink);
        painter.drawText(box.adjusted(6 + static_cast<int>(plus.width() / scale) + 3, 0, -5, 0),
                         Qt::AlignVCenter | Qt::AlignLeft, entry.label);
    }
    painter.setRenderHint(QPainter::Antialiasing, false);
}

QRect TimelineWidget::headerControlRect(const ui::TimelineLayout::Row& row,
                                        HeaderControl control) const {
    if (control == HeaderControl::None) {
        return {};
    }
    // Laid out from the right edge inwards, in the order they are pressed most:
    // remove is the one you least want to hit by accident, so it is the one
    // furthest from the name your eye starts at.
    const int slot = control == HeaderControl::Remove ? 1 : control == HeaderControl::Lock ? 2 : 3;
    const int right = layout_.metrics().headerWidth - kControlMargin;
    const int top = row.top + (row.height - kControlSize) / 2;
    return {right - slot * kControlSize, top, kControlSize, kControlSize};
}

QRect TimelineWidget::cornerControlRect(CornerControl control) const {
    if (control == CornerControl::None) {
        return {};
    }
    QFont pill = font();
    pill.setPointSizeF(8.5);
    const QFontMetrics pillMetrics(pill);
    // The glyph, the gap after it and the padding either side come to 22.
    const int videoWidth = pillMetrics.horizontalAdvance(QStringLiteral("Video")) + 22;
    const int audioWidth = pillMetrics.horizontalAdvance(QStringLiteral("Audio")) + 22;
    const int top = (layout_.metrics().rulerHeight - kPillHeight) / 2;
    if (control == CornerControl::AddVideo) {
        return {8, top, videoWidth, kPillHeight};
    }
    return {8 + videoWidth + kPillGap, top, audioWidth, kPillHeight};
}

TimelineWidget::HeaderHit TimelineWidget::headerHitTest(int x, int y) const {
    const model::Sequence* seq = sequence();
    if (seq == nullptr || !layout_.isInHeaders(x) || y < layout_.metrics().rulerHeight) {
        return {};
    }
    for (const ui::TimelineLayout::Row& row : layout_.rows(*seq)) {
        if (y < row.top || y >= row.top + row.height) {
            continue;
        }
        for (const HeaderControl control :
             {HeaderControl::Mute, HeaderControl::Lock, HeaderControl::Remove}) {
            if (headerControlRect(row, control).contains(x, y)) {
                return {row.track, control};
            }
        }
        return {row.track, HeaderControl::None};
    }
    return {};
}

TimelineWidget::CornerControl TimelineWidget::cornerHitTest(int x, int y) const {
    if (!layout_.isInHeaders(x) || y >= layout_.metrics().rulerHeight) {
        return CornerControl::None;
    }
    for (const CornerControl control : {CornerControl::AddVideo, CornerControl::AddAudio}) {
        if (cornerControlRect(control).contains(x, y)) {
            return control;
        }
    }
    return CornerControl::None;
}

model::TrackId TimelineWidget::headerResizeAt(int x, int y) const {
    const model::Sequence* seq = sequence();
    if (seq == nullptr || !layout_.isInHeaders(x) || y < layout_.metrics().rulerHeight) {
        return {};
    }
    for (const ui::TimelineLayout::Row& row : layout_.rows(*seq)) {
        const int bottom = row.top + row.height;
        if (std::abs(y - bottom) <= kResizeGrabPixels) {
            return row.track;
        }
    }
    return {};
}

void TimelineWidget::updateTrackHeight(int y) {
    if (!resizeTrack_.isValid()) {
        return;
    }
    // Measured from where the gesture started rather than from the last move.
    // A clamped drag would otherwise lose the difference between "the pointer
    // has gone 40 past the limit" and "the pointer is back at the limit", and
    // the row would not start shrinking again until the pointer caught up.
    setTrackHeight(resizeTrack_, resizeStartHeight_ + (y - resizeAnchorY_));
}

void TimelineWidget::updateHeaderHover(int x, int y) {
    const HeaderHit header = headerHitTest(x, y);
    const CornerControl corner = cornerHitTest(x, y);
    if (header.track == hoverHeader_.track && header.control == hoverHeader_.control &&
        corner == hoverCorner_) {
        return;
    }
    hoverHeader_ = header;
    hoverCorner_ = corner;
    update();
}

void TimelineWidget::beginRenameTrack(model::TrackId trackId) {
    const model::Sequence* seq = sequence();
    if (project_ == nullptr || commands_ == nullptr || seq == nullptr) {
        return;
    }
    const model::Track* track = seq->findTrack(trackId);
    if (track == nullptr) {
        return;
    }
    // A locked track is one somebody has said not to change. Renaming it is
    // harmless to the cut, but the lock is a statement about the whole track
    // and honouring it selectively is how a lock stops meaning anything.
    if (track->isLocked()) {
        return;
    }
    const auto row = rowFor(trackId);
    if (!row) {
        return;
    }

    finishRename(true);
    renamingTrack_ = trackId;
    if (renameEditor_ == nullptr) {
        renameEditor_ = new QLineEdit(this);
        renameEditor_->setFrame(false);
        connect(renameEditor_, &QLineEdit::returnPressed, this, [this] { finishRename(true); });
        // Losing focus commits too: clicking away from a half-typed name is
        // how somebody says "that will do", not "throw it away". Escape is
        // what throws it away, and that is handled in the event filter below.
        connect(renameEditor_, &QLineEdit::editingFinished, this, [this] { finishRename(true); });
        renameEditor_->installEventFilter(this);
    }

    const QRect box = trackNameRect(*row, *track);
    renameEditor_->setGeometry(box.adjusted(-2, 3, 2, -3));
    renameEditor_->setFont(font());
    // The badge is not offered for editing: it is the track's position, which
    // is not stored and cannot be typed over. A track called V1 starts empty,
    // because that name is the badge repeating itself.
    const QString name = QString::fromStdString(track->name());
    renameEditor_->setText(name.compare(trackBadge(*row), Qt::CaseInsensitive) == 0 ? QString{}
                                                                                    : name);
    renameEditor_->setPlaceholderText(QStringLiteral("Track name"));
    renameEditor_->selectAll();
    renameEditor_->show();
    renameEditor_->setFocus(Qt::MouseFocusReason);
    update();
}

void TimelineWidget::finishRename(bool keep) {
    if (renameEditor_ == nullptr || !renamingTrack_.isValid()) {
        return;
    }
    // Cleared first, so the editingFinished that hiding provokes finds nothing
    // to do rather than re-entering this and committing twice.
    const model::TrackId trackId = renamingTrack_;
    const QString typed = renameEditor_->text().trimmed();
    renamingTrack_ = {};
    renameEditor_->hide();
    setFocus();

    const model::Sequence* seq = sequence();
    if (!keep || project_ == nullptr || commands_ == nullptr || seq == nullptr) {
        update();
        return;
    }
    const model::Track* track = seq->findTrack(trackId);
    if (track == nullptr || typed.toStdString() == track->name()) {
        update();
        return;
    }
    auto built = edit::makeRenameTrack(*project_, sequenceId_, trackId, typed.toStdString());
    if (!built) {
        update();
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    emit edited();
    update();
}

void TimelineWidget::toggleTrackMute(model::TrackId trackId) {
    const model::Sequence* seq = sequence();
    if (project_ == nullptr || commands_ == nullptr || seq == nullptr) {
        return;
    }
    const model::Track* track = seq->findTrack(trackId);
    if (track == nullptr) {
        return;
    }
    // The whole strip goes through, not just the flag: the operation sets four
    // values, and passing defaults for the other three would silently reset a
    // fader somebody placed.
    edit::TrackState state;
    state.muted = !track->isMuted();
    state.soloed = track->isSoloed();
    state.gainDb = track->gainDb();
    state.pan = track->pan();
    auto built = edit::makeSetTrackState(*project_, sequenceId_, trackId, state);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    emit edited();
    update();
}

void TimelineWidget::toggleTrackLock(model::TrackId trackId) {
    const model::Sequence* seq = sequence();
    if (project_ == nullptr || commands_ == nullptr || seq == nullptr) {
        return;
    }
    const model::Track* track = seq->findTrack(trackId);
    if (track == nullptr) {
        return;
    }
    // Sync lock is carried through untouched. It is a separate idea -- whether
    // the track shifts under a ripple -- and this button is not about it.
    auto built = edit::makeSetTrackLock(*project_, sequenceId_, trackId, !track->isLocked(),
                                        track->isSyncLocked());
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    emit edited();
    update();
}

void TimelineWidget::removeTrack(model::TrackId trackId) {
    if (project_ == nullptr || commands_ == nullptr) {
        return;
    }
    // Abandoned rather than committed: a name typed into a track that is about
    // to stop existing is not a rename anybody wants applied on the way out.
    if (renamingTrack_ == trackId) {
        finishRename(false);
    }
    auto built = edit::makeRemoveTrack(*project_, sequenceId_, trackId);
    if (!built) {
        return;
    }
    // Anything selected on the track that is going has to go with it: a
    // selection pointing at a clip the model no longer holds is a dangling
    // reference the next edit would follow.
    std::erase_if(selection_, [trackId](const edit::ClipRef& ref) { return ref.track == trackId; });
    if (selectedTrack_ == trackId) {
        selectedTrack_ = {};
        selected_ = {};
        selectedLink_ = {};
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    announceSelection();
    emit edited();
    emit viewChanged();
    update();
}

void TimelineWidget::addTrack(model::TrackKind kind) {
    if (project_ == nullptr || commands_ == nullptr) {
        return;
    }
    // Adding a track moves the rows, so the editor would be left floating over
    // the wrong one. Commit what is there and let the layout change.
    finishRename(true);
    // Unnamed, in effect: the badge already says V4, and a placeholder name
    // would only be something to clear before typing a real one.
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    const std::size_t count =
        kind == model::TrackKind::Video ? seq->videoTracks().size() : seq->audioTracks().size();
    const std::string name =
        std::string{kind == model::TrackKind::Video ? "V" : "A"} + std::to_string(count + 1);
    auto built = edit::makeAddTrack(*project_, sequenceId_, kind, name);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    emit edited();
    emit viewChanged();
    update();
}

void TimelineWidget::paintClips(QPainter& painter, const ui::TimelineLayout::Row& row) {
    const model::Sequence& seq = *sequence();
    const model::Track* track = seq.findTrack(row.track);
    if (track == nullptr) {
        return;
    }
    const auto& metrics = layout_.metrics();
    const time::TimeRange visible = layout_.visibleRange(seq.frameRate());

    QFont font = painter.font();
    font.setPointSizeF(9.5);
    painter.setFont(font);

    // Culled to what is on screen. This is the difference between a timeline
    // that stays responsive on a long cut and one that does not.
    for (const model::Clip* clip : track->clipsIn(visible)) {
        const double startX = layout_.xForTime(clip->start());
        const double endX = layout_.xForTime(clip->endExclusive());
        QRectF body(startX, row.top + 2.0, std::max(1.0, endX - startX), row.height - 4.0);
        // Clamp to the content area so a clip starting off-screen still paints
        // its visible part without drawing over the headers.
        if (body.left() < metrics.headerWidth) {
            body.setLeft(metrics.headerWidth);
        }
        if (body.width() <= 0.0) {
            continue;
        }

        const ClipPaint paint = clipPaint(row.kind, *clip);
        const QColor fill = clip->enabled ? paint.body : paint.body.darker(150);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(body, 5.0, 5.0);

        // A strip of the clip's own colour along its top. At the widths a busy
        // timeline actually paints at, the strip is legible when the name is
        // not: it is what makes a track readable at a glance.
        //
        // Clipped to the body's own rounded path rather than to its bounding
        // rectangle, so three pixels of strip still follow the corner radius
        // instead of squaring off the top of every clip.
        if (body.height() > 10.0) {
            QPainterPath rounded;
            rounded.addRoundedRect(body, 5.0, 5.0);
            painter.setClipPath(rounded);
            painter.fillRect(QRectF(body.left(), body.top(), body.width(), 3.0),
                             clip->enabled ? paint.strip : paint.strip.darker(160));
            painter.setClipping(false);
        }
        painter.setBrush(Qt::NoBrush);

        const bool hasFrames =
            row.kind == model::TrackKind::Video && paintFilmstrip(painter, *clip, body);

        // The whole link group is outlined, not just the clip clicked on:
        // an edit is going to move all of them, so all of them should look
        // selected.
        const bool selected = isSelected(clip->id);
        const bool isLinkedToSelection =
            !selected && clip->link.isValid() && clip->link == selectedLink_;

        if (selected || isLinkedToSelection) {
            // A dark line outside the bright one, so the ring reads against a
            // neighbour of the same family butted up against it -- which on a
            // cut is every clip either side.
            if (selected) {
                painter.setPen(QPen(theme::bg(), 1.0));
                painter.drawRoundedRect(body.adjusted(-0.5, -0.5, 0.5, 0.5), 5.5, 5.5);
            }
            painter.setPen(QPen(paint.ring, selected ? 1.5 : 1));
            painter.drawRoundedRect(body.adjusted(0.75, 0.75, -0.75, -0.75), 4.5, 4.5);
        } else {
            painter.setPen(theme::textAt(0.10));
            painter.drawRoundedRect(body.adjusted(0.5, 0.5, -0.5, -0.5), 4.5, 4.5);
        }
        painter.setRenderHint(QPainter::Antialiasing, false);

        if (row.kind == model::TrackKind::Audio) {
            paintWaveform(painter, *clip, body, paint.strip);
        }

        // Linked to something: said on the clip, because the consequence --
        // that editing this also edits its partner -- is a surprise otherwise,
        // and the only other place it shows is the outline that appears on
        // the partner once this one is selected.
        if (clip->link.isValid() && body.width() > 30.0 && body.height() > 18.0) {
            QColor ink = paint.label;
            ink.setAlpha(190);
            const QPixmap& badge = linkBadge(ink);
            const QPointF at(body.right() - badge.width() / badge.devicePixelRatio() - 4.0,
                             body.top() + 5.0);
            // Over a frame, a light glyph on a light shot is invisible. The
            // same shadow the design puts under the clip's name, drawn as an
            // offset copy because Qt has none for a pixmap.
            if (hasFrames) {
                painter.drawPixmap(at + QPointF(0.0, 1.0), linkBadge(kBadgeShadow));
            }
            painter.drawPixmap(at, badge);
        }

        if (body.width() > 28.0) {
            // Over a picture the label needs its own ground. A short gradient
            // rather than a bar: the design shades the text itself, and a hard
            // edge across every clip would read as a second strip.
            if (hasFrames) {
                QLinearGradient scrim(body.left(), body.top(), body.left(), body.top() + 22.0);
                scrim.setColorAt(0.0, QColor(0, 0, 0, 165));
                scrim.setColorAt(1.0, QColor(0, 0, 0, 0));
                QPainterPath rounded;
                rounded.addRoundedRect(body, 5.0, 5.0);
                painter.save();
                painter.setClipPath(rounded);
                painter.fillRect(
                    QRectF(body.left(), body.top(), body.width(), std::min(22.0, body.height())),
                    scrim);
                painter.restore();
            }
            painter.setPen(paint.label);
            // Along the top, under the strip, rather than through the middle:
            // the middle is where the waveform is, and a name drawn over an
            // envelope is unreadable in both directions.
            painter.drawText(body.adjusted(6, 5, -6, 0), Qt::AlignTop | Qt::AlignLeft,
                             QString::fromStdString(clip->name));
        }

        // A clip carrying effects says so, in the corner the link badge does
        // not use. Which effects is the Effect Controls panel's job; all a
        // timeline has room to say is that there are some.
        if (!clip->effects.empty() && body.width() > 30.0 && body.height() > 18.0) {
            QColor ink = paint.strip;
            ink.setAlpha(220);
            const QPixmap& badge = effectsBadge(ink);
            const QPointF at(body.right() - badge.width() / badge.devicePixelRatio() - 4.0,
                             body.bottom() - badge.height() / badge.devicePixelRatio() - 3.0);
            if (hasFrames) {
                painter.drawPixmap(at + QPointF(0.0, 1.0), effectsBadge(kBadgeShadow));
            }
            painter.drawPixmap(at, badge);
        }

        paintKeyframes(painter, *clip, body);
    }
}

void TimelineWidget::dragKeyframeTo(int x) {
    const model::Sequence* seq = sequence();
    if (seq == nullptr || project_ == nullptr || commands_ == nullptr ||
        !keyframeDrag_.clip.isValid()) {
        return;
    }
    const model::Track* track = seq->findTrack(keyframeDrag_.track);
    const model::Clip* clip = track != nullptr ? track->find(keyframeDrag_.clip) : nullptr;
    if (clip == nullptr) {
        return;
    }

    // Clamped to the clip. A keyframe outside the clip's own range is
    // unreachable: nothing samples the curve there, so it could never be
    // grabbed again or seen to do anything.
    time::RationalTime when = layout_.timeForX(x, seq->frameRate());
    const time::RationalTime lastFrame =
        clip->endExclusive() - time::RationalTime{1, clip->start().rate()};
    when = std::clamp(when, clip->start(), lastFrame);

    // Keyframes are placed against the un-remapped mapping, so dragging one
    // has to speak the same coordinates.
    const time::RationalTime target = clip->baseSourceTimeAt(when);
    if (target == keyframeDrag_.time) {
        return;
    }
    auto built = edit::makeMoveKeyframesAt(*project_, {sequenceId_, keyframeDrag_.track},
                                           keyframeDrag_.clip, keyframeDrag_.time, target);
    if (!built) {
        return;  // something is already there; leave it where it was
    }
    commands_->execute(*project_, std::move(*built));
    // The drag now follows the keyframe to its new time, or the next move would
    // look for it where it no longer is.
    keyframeDrag_.time = target;
    emit edited();
    update();
}

void TimelineWidget::paintKeyframes(QPainter& painter, const model::Clip& clip,
                                    const QRectF& body) {
    if (clip.animation.empty()) {
        return;
    }
    const double lane = layout_.keyframeLaneHeight();
    const double centreY = body.bottom() - (lane / 2.0);
    const double half = (lane / 2.0) - 1.0;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (const time::RationalTime& at : ui::TimelineLayout::keyframeTimes(clip)) {
        const double x = layout_.xForTime(clip.timelineTimeOf(at));
        if (x < body.left() - half || x > body.right() + half) {
            continue;
        }
        const bool held = keyframeDrag_.clip == clip.id && keyframeDrag_.time == at;
        // A diamond, the shape every editor uses for a keyframe, filled when it
        // is the one being dragged so the pointer is not the only clue.
        const QPointF points[4] = {
            {x, centreY - half}, {x + half, centreY}, {x, centreY + half}, {x - half, centreY}};
        painter.setPen(QPen(kKeyframeOutline, 1));
        painter.setBrush(held ? kKeyframeHeld : kKeyframe);
        painter.drawPolygon(points, 4);
    }
    painter.restore();
}

/// Zoom invalidates every filmstrip cell: the cells are a fixed width in
/// pixels, so a new zoom means new source times for all of them. Whatever was
/// queued for the old scale is work nobody will look at.
void TimelineWidget::discardQueuedThumbnails() {
    if (thumbnails_ != nullptr) {
        thumbnails_->dropPending();
    }
}

void TimelineWidget::setThumbnailCache(ThumbnailCache* cache) {
    if (thumbnails_ == cache) {
        return;
    }
    if (thumbnails_ != nullptr) {
        disconnect(thumbnails_, nullptr, this, nullptr);
    }
    thumbnails_ = cache;
    if (thumbnails_ != nullptr) {
        // Qt coalesces update() calls, so a burst of frames arriving together
        // is still one repaint.
        connect(thumbnails_, &ThumbnailCache::ready, this, qOverload<>(&TimelineWidget::update));
    }
    update();
}

void TimelineWidget::setWaveform(model::MediaRefId media,
                                 std::shared_ptr<const media::Waveform> waveform) {
    waveforms_[media.value()] = std::move(waveform);
    update();
}

void TimelineWidget::paintWaveform(QPainter& painter, const model::Clip& clip, const QRectF& body,
                                   const QColor& colour) {
    const auto found = waveforms_.find(clip.source.value());
    if (found == waveforms_.end() || found->second == nullptr) {
        return;
    }
    const media::Waveform& waveform = *found->second;
    if (!waveform.isValid() || waveform.bucketCount() == 0) {
        return;
    }
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return;
    }

    // Below the strip and the name, so the three things a clip shows do not
    // sit on top of each other.
    const QRectF field = body.adjusted(0, 12, 0, -2);
    const double midY = field.center().y();
    const double halfHeight = std::max(2.0, field.height() * 0.45);
    QColor ink = colour;
    ink.setAlpha(205);
    painter.setPen(ink);

    // One column per pixel, resolved through the clip's own source mapping --
    // so a trimmed clip shows the part of the waveform it actually plays, and a
    // clip moved along the timeline takes its waveform with it, without any of
    // that being special-cased here.
    const auto from = static_cast<int>(std::floor(body.left()));
    const auto to = static_cast<int>(std::ceil(body.right()));
    for (int x = from; x < to; ++x) {
        const time::RationalTime timelineTime = layout_.timeForX(x, seq->frameRate());
        if (!clip.timelineRange.contains(timelineTime)) {
            continue;
        }
        const time::RationalTime sourceTime = clip.baseSourceTimeAt(timelineTime);
        const std::int64_t sample = sourceTime.rescaledTo(waveform.sampleRate()).frames();
        const std::int64_t bucket = sample / waveform.samplesPerBucket();
        if (bucket < 0 || bucket >= waveform.bucketCount()) {
            continue;
        }

        // Channels folded together: at track height there is no room to show
        // them apart, and the envelope of the loudest is what matters.
        float minimum = 0.0F;
        float maximum = 0.0F;
        for (std::int32_t c = 0; c < waveform.channelCount(); ++c) {
            minimum = std::min(minimum, waveform.at(c, bucket).minimum);
            maximum = std::max(maximum, waveform.at(c, bucket).maximum);
        }
        painter.drawLine(QPointF(x, midY - static_cast<double>(maximum) * halfHeight),
                         QPointF(x, midY - static_cast<double>(minimum) * halfHeight));
    }
}

bool TimelineWidget::paintFilmstrip(QPainter& painter, const model::Clip& clip,
                                    const QRectF& body) {
    const model::Sequence* seq = sequence();
    if (thumbnails_ == nullptr || project_ == nullptr || seq == nullptr) {
        return false;
    }
    // Generated pictures have no media to read, and a nested sequence would
    // have to be composited rather than decoded -- which is the program
    // monitor's job, not a thumbnail's.
    if (clip.graphic.kind != model::GraphicKind::None || clip.nested.isValid()) {
        return false;
    }
    const model::MediaRef* media = project_->findMedia(clip.activeSource());
    if (media == nullptr || media->info.videoStreams.empty()) {
        return false;
    }

    // Cells at the source's own aspect, so the frames are not stretched. The
    // clamp is for the extremes -- a very tall body would otherwise ask for
    // cells wider than most clips, and every clip would show one squeezed frame
    // instead of a strip.
    const media::VideoStreamInfo& video = media->info.videoStreams.front();
    if (video.width <= 0 || video.height <= 0) {
        return false;
    }
    const QRectF field = body.adjusted(0, 3, 0, 0);
    if (field.height() < 12.0) {
        return false;
    }
    const double aspect = static_cast<double>(video.width) / static_cast<double>(video.height);
    const double cellWidth = std::clamp(field.height() * aspect, 24.0, 160.0);
    if (field.width() < cellWidth * 0.75) {
        return false;
    }

    // Anchored to the clip's own left edge rather than to the viewport, so a
    // cell shows the same frame however the timeline is scrolled. The clip's
    // true left is used even when it is off screen, which is what keeps the
    // strip from sliding as a long clip scrolls past.
    const double clipLeft = layout_.xForTime(clip.start());
    const int first = static_cast<int>(std::floor((field.left() - clipLeft) / cellWidth));
    const int last = static_cast<int>(std::ceil((field.right() - clipLeft) / cellWidth));

    QPainterPath rounded;
    rounded.addRoundedRect(body, 5.0, 5.0);
    painter.save();
    painter.setClipPath(rounded);

    bool drewAny = false;
    for (int cell = std::max(0, first); cell <= last; ++cell) {
        const QRectF box(clipLeft + cell * cellWidth, field.top(), cellWidth, field.height());
        if (box.right() < field.left() || box.left() > field.right()) {
            continue;
        }
        // The middle of the cell, not its leading edge: a frame grabbed at the
        // very start of a cut is often the tail of a dissolve.
        //
        // Clamped into the clip rather than skipped when it falls outside. The
        // last cell of a clip is usually a partial one whose centre is past the
        // out point, and skipping it leaves a bare sliver of body colour at the
        // end of every clip -- which reads as a gap in the media rather than as
        // the arithmetic of a fixed cell width.
        time::RationalTime at = layout_.timeForX(box.center().x(), seq->frameRate());
        const time::RationalTime lastFrame =
            clip.endExclusive() - time::RationalTime{1, seq->frameRate()};
        at = std::clamp(at, clip.start(), lastFrame);
        if (at < clip.start()) {
            continue;
        }
        // Through the project rather than off the media reference, so a
        // filmstrip reads the proxy when proxies are on. Decoding the original
        // for a picture 50 pixels tall is the exact cost proxies exist to
        // avoid, and it is paid on every clip in view.
        const QImage frame =
            thumbnails_->lookup(project_->resolvedPath(*media), clip.baseSourceTimeAt(at),
                                static_cast<int>(field.height()));
        if (frame.isNull()) {
            continue;
        }
        // Cropped to the cell rather than letterboxed into it: a strip of
        // frames with black bars between them reads as gaps in the clip.
        const QRectF source(std::max(0.0, (frame.width() - box.width()) / 2.0), 0.0,
                            std::min<double>(frame.width(), box.width()), frame.height());
        painter.drawImage(box, frame, source);
        painter.setPen(QPen(QColor(0, 0, 0, 90), 1.0));
        painter.drawLine(QPointF(box.right(), box.top()), QPointF(box.right(), box.bottom()));
        drewAny = true;
    }

    painter.restore();
    return drewAny;
}

void TimelineWidget::paintTransitions(QPainter& painter, const ui::TimelineLayout::Row& row) {
    const model::Sequence& seq = *sequence();
    const model::Track* track = seq.findTrack(row.track);
    if (track == nullptr) {
        return;
    }
    const auto& metrics = layout_.metrics();

    for (const model::Transition& transition : track->transitions()) {
        const double startX = layout_.xForTime(transition.range.start());
        const double endX = layout_.xForTime(transition.range.endExclusive());
        QRectF box(startX, row.top + 2.0, std::max(2.0, endX - startX), row.height - 4.0);
        if (box.right() < metrics.headerWidth) {
            continue;
        }
        if (box.left() < metrics.headerWidth) {
            box.setLeft(metrics.headerWidth);
        }

        painter.fillRect(box,
                         QColor(kTransition.red(), kTransition.green(), kTransition.blue(), 90));
        painter.setPen(kTransition);
        painter.drawRect(box.adjusted(0.5, 0.5, -0.5, -0.5));
        // A diagonal, the shape every editor draws for a dissolve.
        painter.drawLine(box.bottomLeft(), box.topRight());
    }
}

void TimelineWidget::paintPlayhead(QPainter& painter) {
    const auto& metrics = layout_.metrics();
    const double x = layout_.xForTime(playhead_);
    if (x < metrics.headerWidth) {
        return;
    }
    painter.setRenderHint(QPainter::Antialiasing, true);

    // A glow either side of the line before the line itself. Qt has no
    // box-shadow, so the design's is drawn as a pair of widening, fading
    // strokes -- which is what a one-pixel line needs to stay findable against
    // a busy cut without being thickened into a bar.
    QColor glow = kPlayhead;
    for (const auto& [width, alpha] : {std::pair{5.0, 26}, std::pair{3.0, 52}}) {
        glow.setAlpha(alpha);
        painter.setPen(QPen(glow, width));
        painter.drawLine(QPointF(x, 0), QPointF(x, height()));
    }

    painter.setPen(QPen(kPlayhead, 1.0));
    painter.drawLine(QPointF(x, 0), QPointF(x, height()));

    // A head on the playhead, so it can be found and grabbed in the ruler.
    // Squared off at the shoulders and pointed at the tip: a plain triangle
    // narrows to nothing at the top, which is exactly where the pointer has to
    // land to grab it.
    QPolygonF head;
    head << QPointF(x - 6.5, 0.0) << QPointF(x + 6.5, 0.0) << QPointF(x + 6.5, 7.8)
         << QPointF(x, 13.0) << QPointF(x - 6.5, 7.8);
    painter.setBrush(kPlayhead);
    painter.setPen(Qt::NoPen);
    painter.drawPolygon(head);
    painter.setBrush(Qt::NoBrush);
    painter.setRenderHint(QPainter::Antialiasing, false);
}

void TimelineWidget::paintSnapGuide(QPainter& painter) {
    if (!snapMark_.active) {
        return;
    }
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    const double x = layout_.xForTime(snapMark_.time);
    if (x < layout_.metrics().headerWidth || x > width()) {
        return;
    }

    // Down the whole stack of tracks rather than only the one being edited:
    // the alignment being made is *with* the other tracks, and a line that
    // stops at the clip under the pointer shows the half of it that was never
    // in question.
    painter.setRenderHint(QPainter::Antialiasing, false);
    QPen pen{kSnapGuide, 1.0, Qt::DashLine};
    pen.setDashPattern({3.0, 3.0});
    painter.setPen(pen);
    painter.drawLine(QPointF(x, layout_.metrics().rulerHeight), QPointF(x, height()));

    // A tick at the row the time came from, so a cut lined up with V2 says V2
    // rather than leaving you to count. The playhead and the sequence start
    // belong to no track and get a tick in the ruler instead.
    painter.setPen(Qt::NoPen);
    painter.setBrush(kSnapGuide);
    const auto tick = [&](double top, double bottom) {
        painter.drawRect(QRectF(x - 1.5, top, 3.0, bottom - top));
    };
    if (snapMark_.track.isValid()) {
        if (const auto row = rowFor(snapMark_.track)) {
            tick(row->top + 2.0, row->top + row->height - 2.0);
        }
    } else {
        tick(layout_.metrics().rulerHeight - 5.0, layout_.metrics().rulerHeight);
    }
    painter.setBrush(Qt::NoBrush);
}

void TimelineWidget::paintBladePreview(QPainter& painter) {
    if (!bladeMark_.active) {
        return;
    }
    const auto row = rowFor(bladeMark_.track);
    if (!row) {
        return;
    }
    const double x = layout_.xForTime(bladeMark_.time);
    if (x < layout_.metrics().headerWidth || x > width()) {
        return;
    }

    // Solid, and only across the track that would actually be cut: this is the
    // edit itself, where the dashed guide beside it is only an alignment.
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(kBladeLine, 1.0));
    painter.drawLine(QPointF(x, row->top + 1.0), QPointF(x, row->top + row->height - 1.0));

    // Two nibs, top and bottom, so the line reads as a cut rather than as a
    // second playhead.
    painter.setPen(Qt::NoPen);
    painter.setBrush(kBladeLine);
    QPolygonF top;
    top << QPointF(x - 3.5, row->top + 1.0) << QPointF(x + 3.5, row->top + 1.0)
        << QPointF(x, row->top + 6.0);
    painter.drawPolygon(top);
    QPolygonF bottom;
    const double base = row->top + row->height - 1.0;
    bottom << QPointF(x - 3.5, base) << QPointF(x + 3.5, base) << QPointF(x, base - 5.0);
    painter.drawPolygon(bottom);
    painter.setBrush(Qt::NoBrush);
    painter.setRenderHint(QPainter::Antialiasing, false);
}

// --- Input ------------------------------------------------------------------

time::RationalTime TimelineWidget::maybeSnap(const time::RationalTime& t, model::ClipId ignoring,
                                             bool includePlayhead) {
    const model::Sequence* seq = sequence();
    if (seq == nullptr || !snapEnabled_) {
        snapMark_ = {};
        return t;
    }
    // A fixed pixel radius becomes a shrinking time radius as you zoom in,
    // which is what makes snapping feel helpful rather than obstructive.
    const double thresholdSeconds = 10.0 / layout_.metrics().pixelsPerSecond;
    const auto threshold = time::RationalTime::fromSeconds(
        time::Rational::approximate(thresholdSeconds), seq->frameRate());

    const auto result =
        edit::snapTime(*seq, t, threshold, ignoring, includePlayhead ? &playhead_ : nullptr);
    // Kept rather than discarded: `snapTime` returns what it latched onto and
    // which track it came from precisely so this can be drawn, and until now
    // the answer was thrown away at the call site.
    snapMark_ = SnapMark{result.snapped(), result.time, result.kind, result.track};
    return result.time;
}

void TimelineWidget::clearGestureMarks() {
    if (!snapMark_.active && !bladeMark_.active) {
        return;
    }
    snapMark_ = {};
    bladeMark_ = {};
    update();
}

void TimelineWidget::scrubTo(int x) {
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    auto position = layout_.timeForX(x, seq->frameRate());
    // Without the playhead among the candidates: it is the thing being moved,
    // and it sits at zero distance from itself, so including it made every
    // scrub shorter than the snap radius a scrub that went nowhere.
    position = maybeSnap(position, {}, false);
    const std::int64_t last = std::max<std::int64_t>(0, seq->duration().frames() - 1);
    position =
        time::RationalTime{std::clamp<std::int64_t>(position.frames(), 0, last), seq->frameRate()};
    setPlayhead(position);
    emit playheadMoved(position);
}

void TimelineWidget::mousePressEvent(QMouseEvent* event) {
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    // Left button only. Everything below acts -- toggles a track's mute, starts
    // a drag, cuts -- and a right-click is a request to be told what something
    // does, not to do it. Without this, right-clicking the mute icon would both
    // silence the track and open the menu offering to silence it.
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    setFocus();
    const int x = static_cast<int>(event->position().x());
    const int y = static_cast<int>(event->position().y());

    // Hand and Zoom are about the view rather than the cut, so they act
    // anywhere -- over the ruler and over the headers included -- and are asked
    // before either of those has a chance to mean something else.
    if (tool_ == Tool::Hand || tool_ == Tool::Zoom) {
        pressAt_ = QPoint(x, y);
        static_cast<void>(pressWithTool(nullptr, x, y, event->modifiers()));
        return;
    }

    // The corner box shares the ruler's band of the widget, so it is asked
    // first -- otherwise pressing "+ Audio" would scrub.
    if (const CornerControl corner = cornerHitTest(x, y); corner != CornerControl::None) {
        addTrack(corner == CornerControl::AddVideo ? model::TrackKind::Video
                                                   : model::TrackKind::Audio);
        return;
    }
    if (layout_.isInRuler(x, y)) {
        drag_ = Drag::Scrub;
        scrubTo(x);
        return;
    }
    if (layout_.isInHeaders(x)) {
        // The boundary first. It is a thin band at the very bottom of a row and
        // the buttons are centred in it, so the two do not overlap until a row
        // is at its minimum -- and there, resizing is what the pointer is for.
        if (const model::TrackId track = headerResizeAt(x, y); track.isValid()) {
            resizeTrack_ = track;
            resizeAnchorY_ = y;
            resizeStartHeight_ = trackHeight(track);
            drag_ = Drag::TrackHeight;
            return;
        }
        const HeaderHit hit = headerHitTest(x, y);
        switch (hit.control) {
            case HeaderControl::Mute:
                toggleTrackMute(hit.track);
                break;
            case HeaderControl::Lock:
                toggleTrackLock(hit.track);
                break;
            case HeaderControl::Remove:
                removeTrack(hit.track);
                break;
            case HeaderControl::None:
                // The header, but not a button on it: that is picking the
                // track. Clips first, so the panel is told the clip selection
                // is empty before it is told what replaced it.
                if (hit.track.isValid()) {
                    selection_.clear();
                    announceSelection();
                    headSelected_ = hit.track;
                    emit trackSelected(headSelected_);
                    update();
                }
                break;
        }
        return;
    }

    pressAt_ = QPoint(x, y);

    // Keyframes are tested first. They live inside a clip, so testing the clip
    // first would mean every keyframe press started a clip drag instead. Only
    // under Select: a diamond is a small target, and having the blade cut
    // everywhere except on top of one would be a cut that sometimes did
    // nothing.
    if (tool_ == Tool::Select) {
        if (const auto key = layout_.hitTestKeyframe(*seq, x, y)) {
            // Alt deletes it. There is no keyframe *selection* — a selection model
            // exists for clips and building a second one just so Delete has
            // something to act on is the half-built trap multi-selection was
            // deferred to avoid. A modifier on the thing itself needs no state.
            if (event->modifiers().testFlag(Qt::AltModifier)) {
                if (commands_ != nullptr) {
                    auto built = edit::makeRemoveKeyframesAt(*project_, {sequenceId_, key->track},
                                                             key->clip, key->time);
                    if (built) {
                        commands_->execute(*project_, std::move(*built));
                        commands_->breakMerge();
                        emit edited();
                    }
                }
                update();
                return;
            }
            keyframeDrag_ = KeyframeDrag{key->track, key->clip, key->time};
            drag_ = Drag::Keyframe;
            update();
            return;
        }
    }

    const auto hit = layout_.hitTest(*seq, x, y);

    if (hit && pressWithTool(&*hit, x, y, event->modifiers())) {
        update();
        return;
    }

    if (!hit) {
        // Not yet a band and not yet a scrub. Which it becomes depends on
        // whether the pointer moves: a click on empty timeline should still put
        // the playhead there, and a drag should select.
        drag_ = Drag::MaybeBand;
        band_ = QRect(pressAt_, pressAt_);
        if (!event->modifiers().testFlag(Qt::ShiftModifier)) {
            selection_.clear();
            announceSelection();
        }
        return;
    }

    if (event->modifiers().testFlag(Qt::ShiftModifier)) {
        toggleSelected(*hit);
        return;
    }
    // Clicking something already selected keeps the selection, so a set can be
    // dragged by any of its members -- but the one under the pointer becomes
    // the primary. Everything a drag then does is aimed at the primary
    // selection: `beginDrag` anchors the trim to the clip that was pressed,
    // while `updateTrim` and `updateSlip` act on `selected_`. Leaving the
    // primary alone here let those two disagree, and a trim aimed at one clip
    // was applied to another -- or, when the delta made no sense against the
    // other clip's edges, refused, so the drag silently did nothing.
    if (!isSelected(hit->clip)) {
        selectOnly(*hit);
    } else {
        makePrimary(*hit);
    }
    // Alt turns a trim into a ripple trim, closing the gap it would leave
    // instead of opening one.
    beginDrag(*hit, x, event->modifiers().testFlag(Qt::AltModifier));
    update();
}

void TimelineWidget::beginDrag(const ui::TimelineLayout::Hit& hit, int x, bool ripple) {
    const model::Sequence* seq = sequence();
    const model::Track* track = seq->findTrack(hit.track);
    if (track == nullptr) {
        return;
    }
    const model::Clip* clip = track->find(hit.clip);
    if (clip == nullptr) {
        return;
    }
    rippleTrim_ = ripple;

    switch (hit.part) {
        case ui::TimelineLayout::Part::InEdge:
            drag_ = Drag::TrimIn;
            trimAnchor_ = clip->start();
            return;
        case ui::TimelineLayout::Part::OutEdge:
            drag_ = Drag::TrimOut;
            trimAnchor_ = clip->endExclusive();
            return;
        case ui::TimelineLayout::Part::Body:
        default:
            // Remember where in the clip the pointer landed, so it does not
            // leap to put its start under the cursor.
            grabOffset_ = layout_.timeForX(x, seq->frameRate()) - clip->start();
            drag_ = Drag::MoveClip;
            return;
    }
}

void TimelineWidget::updateTrim(int x) {
    model::Sequence* seq = project_->findSequence(sequenceId_);
    if (seq == nullptr || !selected_.isValid() || commands_ == nullptr) {
        return;
    }
    const bool trimmingIn = drag_ == Drag::TrimIn;

    auto wanted = layout_.timeForX(x, seq->frameRate());
    wanted = maybeSnap(wanted, selected_);

    const time::RationalTime delta = wanted - trimAnchor_;
    if (delta.isZero()) {
        return;
    }

    const edit::EditTarget target{sequenceId_, selectedTrack_};
    const edit::Edge edge = trimmingIn ? edit::Edge::In : edit::Edge::Out;
    auto built = rippleTrim_ ? edit::makeRippleTrim(*project_, target, selected_, edge, delta)
                             : edit::makeTrim(*project_, target, selected_, edge, delta);
    if (!built) {
        // Refused -- out of source, or into a neighbour. Leave the clip alone;
        // the pointer can keep moving and the trim resumes when it becomes
        // legal again.
        return;
    }
    commands_->execute(*project_, std::move(*built));

    // Re-read where the edge actually ended up. A trim can be clamped, and
    // measuring the next delta from where the pointer wanted rather than from
    // where the edge landed would accumulate the difference.
    if (const model::Track* track = seq->findTrack(selectedTrack_)) {
        if (const model::Clip* clip = track->find(selected_)) {
            trimAnchor_ = trimmingIn ? clip->start() : clip->endExclusive();
        }
    }

    emit edited();
    update();
}

bool TimelineWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == renameEditor_ && event->type() == QEvent::KeyPress) {
        if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            finishRename(false);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TimelineWidget::trackHeaderMenu(model::TrackId trackId, const QPoint& at) {
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    const model::Track* track = seq->findTrack(trackId);
    const auto row = rowFor(trackId);
    if (track == nullptr || !row) {
        return;
    }
    const bool isAudio = row->kind == model::TrackKind::Audio;
    const bool muted = track->isMuted();
    const bool locked = track->isLocked();

    QMenu menu;
    QAction* rename = menu.addAction(QStringLiteral("Rename…"));
    // Offered but refused would be worse than not offered: the rename editor
    // declines a locked track, so the menu says so rather than doing nothing
    // when it is picked.
    rename->setEnabled(!locked);
    menu.addSeparator();
    // A video track is hidden and an audio track is muted. One flag in the
    // model, because "leave this track out" is one idea -- but naming it for
    // the wrong sense is how a menu item stops being findable.
    QAction* silence =
        menu.addAction(isAudio ? (muted ? QStringLiteral("Unmute") : QStringLiteral("Mute"))
                               : (muted ? QStringLiteral("Show") : QStringLiteral("Hide")));
    QAction* lock = menu.addAction(locked ? QStringLiteral("Unlock") : QStringLiteral("Lock"));
    menu.addSeparator();
    QAction* remove = menu.addAction(isAudio ? QStringLiteral("Remove audio track")
                                             : QStringLiteral("Remove video track"));

    const QAction* picked = menu.exec(at);
    if (picked == rename) {
        beginRenameTrack(trackId);
    } else if (picked == silence) {
        toggleTrackMute(trackId);
    } else if (picked == lock) {
        toggleTrackLock(trackId);
    } else if (picked == remove) {
        removeTrack(trackId);
    }
}

void TimelineWidget::clipMenu(const ui::TimelineLayout::Hit& hit, const QPoint& at) {
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    const model::Track* track = seq->findTrack(hit.track);
    const auto row = rowFor(hit.track);
    if (track == nullptr || !row) {
        return;
    }
    const model::Clip* clip = track->find(hit.clip);
    if (clip == nullptr) {
        return;
    }

    // The pointer decides what the menu is about. Selecting before the menu
    // opens also means the item can act on the selection, which is what every
    // other route into this operation already does.
    selectOnly(hit);

    // Only a piece of decoded picture has scene changes in it. A shape, a
    // title and a nested sequence are all generated rather than shot, and a
    // sound has no picture at all -- so the item is absent rather than
    // disabled, because there is nothing about them it could ever do.
    const bool analysable =
        row->kind == model::TrackKind::Video && !clip->nested.isValid() && !clip->graphic.isSet();

    QMenu menu;
    QAction* detect = nullptr;
    if (analysable) {
        detect = menu.addAction(QStringLiteral("Detect Cuts in This Clip"));
        // Refused by the edit rather than half-done, and an analysis that
        // decodes every frame before being told no is a long wait for nothing.
        detect->setEnabled(!track->isLocked());
    }
    if (menu.isEmpty()) {
        return;
    }

    if (const QAction* picked = menu.exec(at); picked != nullptr && picked == detect) {
        emit detectScenesRequested();
    }
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* event) {
    const HeaderHit hit = headerHitTest(event->pos().x(), event->pos().y());
    if (hit.track.isValid()) {
        // Anywhere in the header, including on the buttons: a right-click on a
        // control is somebody asking what it does, not pressing it.
        event->accept();
        trackHeaderMenu(hit.track, event->globalPos());
        return;
    }

    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        QWidget::contextMenuEvent(event);
        return;
    }
    const auto clip = layout_.hitTest(*seq, event->pos().x(), event->pos().y());
    if (!clip) {
        // Empty timeline, over the rows: the one thing that can be made here
        // out of nothing is a title. Anything else needs something to act on.
        if (event->pos().y() >= layout_.metrics().rulerHeight) {
            event->accept();
            QMenu menu{this};
            QAction* title = menu.addAction("Add Title");
            if (menu.exec(event->globalPos()) == title) {
                emit addTitleRequested();
            }
            return;
        }
        QWidget::contextMenuEvent(event);
        return;
    }
    event->accept();
    clipMenu(*clip, event->globalPos());
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    const int x = static_cast<int>(event->position().x());
    const int y = static_cast<int>(event->position().y());

    // Only the name band. The badge, the buttons and the corner box all mean
    // something else on a double click, and the second click of one has already
    // been delivered as a press.
    // A boundary resets to the height the panel opened with, which is the
    // usual way back from having dragged something to a size you regret.
    if (const model::TrackId track = headerResizeAt(x, y); track.isValid()) {
        // Back to whatever its kind is set to, rather than to a number of its
        // own: a double-click on an edge means "undo what I did to this row".
        clearTrackHeight(track);
        return;
    }

    const HeaderHit hit = headerHitTest(x, y);
    if (hit.control == HeaderControl::None && hit.track.isValid()) {
        if (const auto row = rowFor(hit.track)) {
            const model::Track* track = seq->findTrack(hit.track);
            if (track != nullptr && trackNameRect(*row, *track).contains(x, y)) {
                beginRenameTrack(hit.track);
                return;
            }
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event) {
    const int x = static_cast<int>(event->position().x());
    const int y = static_cast<int>(event->position().y());

    if (drag_ == Drag::None) {
        updateHeaderHover(x, y);
        if (layout_.isInHeaders(x)) {
            // Answered here and gone: nothing further down the function is
            // about the headers, and applyCursor at the end would put the
            // arrow back over a boundary that wants the resize cursor.
            setCursor(headerResizeAt(x, y).isValid() ? Qt::SizeVerCursor : Qt::ArrowCursor);
            return;
        }
    }

    if (drag_ == Drag::TrackHeight) {
        updateTrackHeight(y);
        return;
    }
    if (drag_ == Drag::Keyframe) {
        dragKeyframeTo(x);
        return;
    }
    if (drag_ == Drag::MaybeBand) {
        // A few pixels of slack, so a click with an unsteady hand is still a
        // click.
        if ((QPoint(x, y) - pressAt_).manhattanLength() > 4) {
            drag_ = Drag::Band;
        } else {
            return;
        }
    }
    if (drag_ == Drag::Band) {
        band_ = QRect(pressAt_, QPoint(x, y)).normalized();
        update();
        return;
    }
    if (drag_ == Drag::Scrub) {
        scrubTo(x);
        return;
    }
    if (drag_ == Drag::MoveClip) {
        updateDrag(x, y);
        return;
    }
    if (drag_ == Drag::TrimIn || drag_ == Drag::TrimOut) {
        updateTrim(x);
        return;
    }
    if (drag_ == Drag::Slip) {
        updateSlip(x);
        return;
    }
    if (drag_ == Drag::Pan) {
        updatePan(x);
        return;
    }

    // A cursor that tells you what a click will do, before you make it.
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    const auto hit = layout_.hitTest(*seq, x, y);
    applyCursor(hit ? &*hit : nullptr);
    if (tool_ == Tool::Blade) {
        updateBladeHover(x, y);
    }
}

void TimelineWidget::updateDrag(int x, int y) {
    model::Sequence* seq = project_->findSequence(sequenceId_);
    if (seq == nullptr || !selected_.isValid() || commands_ == nullptr) {
        return;
    }
    const model::Track* track = seq->findTrack(selectedTrack_);
    const model::Clip* clip = track != nullptr ? track->find(selected_) : nullptr;
    if (clip == nullptr) {
        return;
    }

    auto start = layout_.timeForX(x, seq->frameRate()) - grabOffset_;
    start = maybeSnap(start, selected_);
    if (start.frames() < 0) {
        start = time::RationalTime{0, seq->frameRate()};
    }

    // Which row the pointer is over, if the clip may go there.
    //
    // Picture to picture and sound to sound: the two families are not
    // interchangeable -- a sound clip on a video track would draw as an empty
    // block and show nothing -- so a drag that wanders across the boundary
    // keeps the clip on the row it came from rather than refusing to move at
    // all. A locked row is no destination either.
    model::TrackId destination = selectedTrack_;
    if (const auto row = layout_.rowAt(*seq, y); row && row->track != selectedTrack_) {
        const model::Track* wanted = seq->findTrack(row->track);
        if (wanted != nullptr && wanted->kind() == track->kind() && !wanted->isLocked()) {
            destination = row->track;
        }
    }

    if (selection_.size() > 1) {
        // Sideways only, for a set: moving several clips between rows is a
        // different question -- which row does each of them land on -- and one
        // this gesture has no way to ask.
        destination = selectedTrack_;
    }

    // Drawn, not done. What the pointer is over is a proposal until the button
    // comes up; see MovePreview for why a move cannot be applied on the way
    // through.
    movePreview_ = MovePreview{true, destination, start, start - clip->start()};
    update();
}

void TimelineWidget::commitMove() {
    const MovePreview preview = movePreview_;
    movePreview_ = {};
    if (!preview.active || project_ == nullptr || commands_ == nullptr || !selected_.isValid()) {
        return;
    }
    model::Sequence* seq = project_->findSequence(sequenceId_);
    if (seq == nullptr) {
        return;
    }
    const model::Track* track = seq->findTrack(selectedTrack_);
    const model::Clip* clip = track != nullptr ? track->find(selected_) : nullptr;
    if (clip == nullptr) {
        return;
    }

    if (selection_.size() > 1) {
        // The whole set moves by whatever the dragged clip moved, and a set
        // stays on its own rows.
        if (preview.delta.frames() == 0) {
            return;
        }
        auto multi = edit::makeMoveClips(*project_, sequenceId_, selection_, preview.delta);
        if (!multi) {
            return;
        }
        commands_->execute(*project_, std::move(*multi));
        emit edited();
        update();
        return;
    }

    if (preview.track == selectedTrack_ && preview.start == clip->start()) {
        return;  // the pointer moved, the clip would not
    }

    // One command for the gesture, made where the gesture ends. Anything
    // linked to the clip stays on its own track and follows the same shift in
    // time, which is what `makeMove` does with a link group: sound follows
    // picture rather than joining it.
    auto built = edit::makeMove(*project_, {sequenceId_, selectedTrack_}, selected_, preview.track,
                                preview.start);
    if (!built) {
        return;  // the move is not legal; leave the clip where it was
    }
    commands_->execute(*project_, std::move(*built));
    if (preview.track != selectedTrack_) {
        selectedTrack_ = preview.track;
        if (!selection_.empty()) {
            selection_.front().track = preview.track;
        }
        announceSelection();
    }
    emit edited();
    update();
}

void TimelineWidget::finishDrag() {
    if (drag_ == Drag::MoveClip) {
        // The move happens here, at the end of the gesture, rather than on
        // every step of it.
        commitMove();
    }
    movePreview_ = {};
    if (drag_ == Drag::Pan && tool_ == Tool::Hand) {
        setCursor(Qt::OpenHandCursor);
    }
    // Scrub, pan and a track resize change the view rather than the cut, so
    // there is no merge group of theirs to close.
    if (drag_ != Drag::None && drag_ != Drag::Scrub && drag_ != Drag::Pan &&
        drag_ != Drag::TrackHeight && commands_ != nullptr) {
        // Close the merge group, so the next gesture is a separate undo step.
        commands_->breakMerge();
    }
    resizeTrack_ = model::TrackId{};
    drag_ = Drag::None;
    rippleTrim_ = false;
    // The guide belongs to the gesture that made it. Left up, it becomes a
    // line on the timeline that means nothing.
    snapMark_ = {};
    update();
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event) {
    const model::Sequence* seq = sequence();
    if (drag_ == Drag::MaybeBand) {
        // Never became a drag, so it was a click: put the playhead there.
        drag_ = Drag::None;
        scrubTo(static_cast<int>(event->position().x()));
        // The guide belongs to the gesture, and the gesture is over -- these
        // early returns skip finishDrag, which is where that normally happens.
        snapMark_ = {};
        update();
        return;
    }
    if (drag_ == Drag::Keyframe) {
        drag_ = Drag::None;
        keyframeDrag_ = {};
        snapMark_ = {};
        if (commands_ != nullptr) {
            // One drag is one undo step; the next one is a new gesture.
            commands_->breakMerge();
        }
        update();
        return;
    }
    if (drag_ == Drag::Band && seq != nullptr) {
        const auto hits =
            layout_.hitTestRect(*seq, band_.left(), band_.top(), band_.right(), band_.bottom());
        if (!event->modifiers().testFlag(Qt::ShiftModifier)) {
            selection_.clear();
        }
        for (const auto& hit : hits) {
            if (!isSelected(hit.clip)) {
                selection_.push_back(edit::ClipRef{hit.track, hit.clip});
            }
        }
        band_ = QRect();
        drag_ = Drag::None;
        snapMark_ = {};
        announceSelection();
        update();
        return;
    }
    finishDrag();
}

void TimelineWidget::leaveEvent(QEvent* event) {
    // The pointer is somewhere else, so the blade is not about to cut anything
    // and no header button is lit.
    hoverHeader_ = {};
    hoverCorner_ = CornerControl::None;
    clearGestureMarks();
    QWidget::leaveEvent(event);
}

void TimelineWidget::wheelEvent(QWheelEvent* event) {
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0) {
        return;
    }

    if (event->modifiers().testFlag(Qt::ControlModifier) ||
        event->modifiers().testFlag(Qt::MetaModifier)) {
        layout_.zoomBy(std::pow(1.2, steps), event->position().x(), seq->frameRate());
        discardQueuedThumbnails();
    } else {
        // Scroll by a fraction of the visible span, so the feel is the same at
        // every zoom level.
        const time::TimeRange visible = layout_.visibleRange(seq->frameRate());
        const std::int64_t delta = static_cast<std::int64_t>(
            -steps * static_cast<double>(visible.duration().frames()) / 6.0);
        layout_.setScroll(layout_.scroll().rescaledTo(seq->frameRate()) +
                          time::RationalTime{delta, seq->frameRate()});
    }
    emit viewChanged();
    update();
}

bool TimelineWidget::pressWithTool(const ui::TimelineLayout::Hit* hit, int x, int /*y*/,
                                   Qt::KeyboardModifiers modifiers) {
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return false;
    }

    switch (tool_) {
        case Tool::Select:
            return false;

        case Tool::Hand:
            panAnchorX_ = x;
            panAnchorScroll_ = layout_.scroll().rescaledTo(seq->frameRate());
            drag_ = Drag::Pan;
            setCursor(Qt::ClosedHandCursor);
            return true;

        case Tool::Zoom:
            // Alt reverses it, the way it does in every other tool that has
            // one direction and needs two.
            layout_.zoomBy(modifiers.testFlag(Qt::AltModifier) ? 1.0 / 1.6 : 1.6, x,
                           seq->frameRate());
            discardQueuedThumbnails();
            emit viewChanged();
            update();
            return true;

        case Tool::Blade: {
            if (hit == nullptr) {
                return true;
            }
            // Snapped, so a cut aimed at a neighbouring edit point lands on it
            // rather than a frame away from it -- and at exactly the time the
            // preview line was drawn at, since both go through maybeSnap.
            razorAt(hit->track, maybeSnap(layout_.nearestTimeForX(x, seq->frameRate()), {}));
            // The cut just made is an edit point of its own; the preview is
            // now describing a boundary rather than a cut.
            bladeMark_ = {};
            return true;
        }

        case Tool::Trim: {
            if (hit == nullptr) {
                return true;
            }
            const model::Track* track = seq->findTrack(hit->track);
            const model::Clip* clip = track != nullptr ? track->find(hit->clip) : nullptr;
            if (clip == nullptr) {
                return true;
            }
            selectOnly(*hit);
            rippleTrim_ = modifiers.testFlag(Qt::AltModifier);
            // The nearer edge, so the whole clip is a trim handle: that is what
            // picking the tool asked for.
            const double middle =
                (layout_.xForTime(clip->start()) + layout_.xForTime(clip->endExclusive())) / 2.0;
            if (x < middle) {
                drag_ = Drag::TrimIn;
                trimAnchor_ = clip->start();
            } else {
                drag_ = Drag::TrimOut;
                trimAnchor_ = clip->endExclusive();
            }
            return true;
        }

        case Tool::Slip: {
            if (hit == nullptr) {
                return true;
            }
            selectOnly(*hit);
            slipAnchorX_ = x;
            drag_ = Drag::Slip;
            return true;
        }
    }
    return false;
}

void TimelineWidget::updateBladeHover(int x, int y) {
    const model::Sequence* seq = sequence();
    if (seq == nullptr || tool_ != Tool::Blade) {
        return;
    }
    const auto hit = layout_.hitTest(*seq, x, y);
    if (!hit) {
        // Off a clip there is nothing to cut, so there is nothing to promise.
        if (bladeMark_.active || snapMark_.active) {
            bladeMark_ = {};
            snapMark_ = {};
            update();
        }
        return;
    }

    // The same call the press will make, so what is drawn is what will happen
    // rather than a second opinion about it.
    // The same question the press asks, so the line promises where the cut
    // will land rather than where the pointer happens to be standing.
    const time::RationalTime at = maybeSnap(layout_.nearestTimeForX(x, seq->frameRate()), {});
    if (bladeMark_.active && bladeMark_.track == hit->track && bladeMark_.time == at) {
        return;
    }
    bladeMark_ = BladeMark{true, hit->track, at};
    update();
}

void TimelineWidget::updateSlip(int x) {
    model::Sequence* seq = project_ != nullptr ? project_->findSequence(sequenceId_) : nullptr;
    if (seq == nullptr || !selected_.isValid() || commands_ == nullptr) {
        return;
    }
    // Measured in time rather than in pixels, so the gesture means the same
    // thing at every zoom level.
    const time::RationalTime delta =
        layout_.timeForX(x, seq->frameRate()) - layout_.timeForX(slipAnchorX_, seq->frameRate());
    if (delta.frames() == 0) {
        return;
    }
    // Dragging right shows *earlier* source: the content moves with the
    // pointer, which is the half of the gesture somebody is actually watching.
    auto built = edit::makeSlip(*project_, {sequenceId_, selectedTrack_}, selected_, -delta);
    if (!built) {
        return;  // out of source at one end; the drag can carry on the other way
    }
    commands_->execute(*project_, std::move(*built));
    // Only once a whole frame has been consumed, so slow drags at a high zoom
    // accumulate instead of being rounded away one move at a time.
    slipAnchorX_ = x;
    emit edited();
    update();
}

void TimelineWidget::updatePan(int x) {
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    const time::RationalTime moved =
        layout_.timeForX(x, seq->frameRate()) - layout_.timeForX(panAnchorX_, seq->frameRate());
    // The content follows the hand, so the view moves the other way.
    layout_.setScroll(panAnchorScroll_ - moved);
    emit viewChanged();
    update();
}

void TimelineWidget::applyCursor(const ui::TimelineLayout::Hit* hit) {
    switch (tool_) {
        case Tool::Hand:
            setCursor(Qt::OpenHandCursor);
            return;
        case Tool::Zoom:
            setCursor(Qt::CrossCursor);
            return;
        case Tool::Blade:
            setCursor(Qt::SplitHCursor);
            return;
        case Tool::Trim:
        case Tool::Slip:
            setCursor(hit != nullptr ? Qt::SizeHorCursor : Qt::ArrowCursor);
            return;
        case Tool::Select:
        default:
            break;
    }
    if (hit == nullptr) {
        unsetCursor();
        return;
    }
    setCursor(hit->part == ui::TimelineLayout::Part::Body ? Qt::OpenHandCursor : Qt::SizeHorCursor);
}

void TimelineWidget::razorAt(model::TrackId track, const time::RationalTime& at) {
    if (project_ == nullptr || commands_ == nullptr || !track.isValid()) {
        return;
    }
    auto built = edit::makeRazor(*project_, {sequenceId_, track}, at);
    if (!built) {
        // On a boundary, or past the end of the clip: a cut that would produce
        // a zero-length clip is refused rather than approximated.
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    emit edited();
    update();
}

void TimelineWidget::razorAtPlayhead() {
    if (project_ == nullptr || commands_ == nullptr || !selectedTrack_.isValid()) {
        return;
    }
    auto built = edit::makeRazor(*project_, {sequenceId_, selectedTrack_}, playhead_);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    emit edited();
    update();
}

void TimelineWidget::addMarkerAtPlayhead() {
    if (project_ == nullptr || commands_ == nullptr) {
        return;
    }
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    auto built = edit::makeAddMarker(*project_, sequenceId_, playhead_,
                                     time::RationalTime{0, seq->frameRate()}, "Marker");
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    emit edited();
    update();
}

void TimelineWidget::addDissolveAtPlayhead() {
    if (project_ == nullptr || commands_ == nullptr || !selectedTrack_.isValid()) {
        return;
    }
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    // A second, which is what most editors default to and what a dissolve
    // usually wants to be before anyone adjusts it.
    const auto duration = time::RationalTime::fromSeconds(time::Rational{1, 1}, seq->frameRate());
    auto built =
        edit::makeAddCrossDissolve(*project_, {sequenceId_, selectedTrack_}, playhead_, duration);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    emit edited();
    update();
}

bool TimelineWidget::setTransitionKindAtPlayhead(model::TransitionKind kind,
                                                 model::TransitionDirection direction) {
    if (project_ == nullptr || commands_ == nullptr || !selectedTrack_.isValid()) {
        return false;
    }
    const model::Sequence* seq = sequence();
    const model::Track* track = seq != nullptr ? seq->findTrack(selectedTrack_) : nullptr;
    if (track == nullptr) {
        return false;
    }
    const model::Transition* under = track->transitionAt(playhead_);
    if (under == nullptr) {
        return false;
    }
    auto built = edit::makeSetTransitionKind(*project_, {sequenceId_, selectedTrack_}, under->id,
                                             kind, direction);
    if (!built) {
        return false;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    emit edited();
    update();
    return true;
}

void TimelineWidget::selectOnly(model::TrackId track, model::ClipId clip) {
    selection_.clear();
    selection_.push_back(edit::ClipRef{track, clip});
    announceSelection();
    update();
}

void TimelineWidget::selectAlso(model::TrackId track, model::ClipId clip) {
    if (!clip.isValid()) {
        return;
    }
    for (const edit::ClipRef& already : selection_) {
        if (already.clip == clip && already.track == track) {
            return;
        }
    }
    selection_.push_back(edit::ClipRef{track, clip});
    announceSelection();
    update();
}

void TimelineWidget::switchAngle(int angle) {
    const model::Sequence* seq = sequence();
    if (seq == nullptr || project_ == nullptr || commands_ == nullptr || selection_.empty()) {
        return;
    }
    // The primary selection: switching several clips at once would be several
    // cuts in several places, which is not what pressing a number means.
    const edit::ClipRef& target = selection_.front();
    auto built = edit::makeSwitchAngle(*project_, {sequenceId_, target.track}, target.clip, angle,
                                       playhead_);
    if (!built) {
        return;  // no such angle, already live, or the playhead is elsewhere
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    selection_.clear();
    announceSelection();
    emit edited();
    update();
}

void TimelineWidget::removeSelected(bool ripple) {
    if (project_ == nullptr || commands_ == nullptr || selection_.empty()) {
        return;
    }
    auto built = edit::makeRemoveClips(*project_, sequenceId_, selection_, ripple);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    selection_.clear();
    announceSelection();
    emit edited();
    update();
}

void TimelineWidget::keyPressEvent(QKeyEvent* event) {
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        QWidget::keyPressEvent(event);
        return;
    }

    switch (event->key()) {
        case Qt::Key_C:
            razorAtPlayhead();
            return;
        // Snapping and the five tools used to be handled here. They are actions
        // now -- see ui::allActions() -- so they appear in the hotkeys list,
        // can be rebound, and work whatever has focus.
        //
        // Delete and Backspace stay, and are the deliberate exception. They are
        // also registered as actions, for the same discoverability, but a
        // timeline that stops deleting because somebody rebound something is a
        // worse outcome than the duplication: these two keys are what every
        // editor's hands do without looking. The keymap binds one key per
        // action, so the pair could not be expressed there anyway.
        case Qt::Key_Backspace:
        case Qt::Key_Delete:
            removeSelected(event->modifiers().testFlag(Qt::ShiftModifier));
            return;
        case Qt::Key_M:
            addMarkerAtPlayhead();
            return;
        case Qt::Key_D:
            if (event->modifiers().testFlag(Qt::ControlModifier) ||
                event->modifiers().testFlag(Qt::MetaModifier)) {
                addDissolveAtPlayhead();
                return;
            }
            break;
        case Qt::Key_1:
        case Qt::Key_2:
        case Qt::Key_3:
        case Qt::Key_4:
        case Qt::Key_5:
        case Qt::Key_6:
        case Qt::Key_7:
        case Qt::Key_8:
        case Qt::Key_9:
            // Number keys switch angle at the playhead, which is how a
            // multicam is cut: watch it play and press the camera you want.
            switchAngle(event->key() - Qt::Key_1);
            return;
        case Qt::Key_A:
            if (event->modifiers().testFlag(Qt::ControlModifier) ||
                event->modifiers().testFlag(Qt::MetaModifier)) {
                selectAll();
                return;
            }
            break;
        case Qt::Key_Z:
            if (event->modifiers().testFlag(Qt::ControlModifier) ||
                event->modifiers().testFlag(Qt::MetaModifier)) {
                if (event->modifiers().testFlag(Qt::ShiftModifier)) {
                    redo();
                } else {
                    undo();
                }
                return;
            }
            break;
        case Qt::Key_Equal:
        case Qt::Key_Plus:
            zoomBy(1.4);
            return;
        case Qt::Key_Minus:
            zoomBy(1.0 / 1.4);
            return;
        case Qt::Key_Backslash:
            zoomToFit();
            return;
        default:
            break;
    }
    QWidget::keyPressEvent(event);
}

// --- Files dropped from the media pane --------------------------------------
//
// The bin says which file was picked up; where it is let go of says everything
// else. Dropping over a picture row puts the clip on that row, over a sound row
// on that one -- and when the clip would land on top of something already
// there, a new row is made for it rather than overwriting a cut somebody has
// already made. That last rule is what makes the gesture safe enough to be
// quick: the worst a mis-aimed drop can do is add a track.

namespace {

/// The part of a file a dragged bin row stands for, in the file's own time.
///
/// A subclip stands for its range; a file stands for all of it. The same rule
/// the bin's own Append uses, and the only place a subclip means anything --
/// what lands on the timeline is an ordinary clip either way.
std::optional<time::TimeRange> draggedSourceRange(const model::Project& project,
                                                  const MediaDrag& dragged,
                                                  const time::Rational& sequenceRate) {
    const model::MediaRef* ref = project.findMedia(dragged.media);
    if (ref == nullptr || !ref->info.duration.isPositive()) {
        return std::nullopt;
    }
    const media::VideoStreamInfo* video = ref->info.primaryVideo();
    const time::Rational sourceRate = video != nullptr ? video->frameRate : sequenceRate;
    if (const model::Subclip* subclip = project.findSubclip(dragged.subclip)) {
        return subclip->range.rescaledTo(sourceRate);
    }
    return time::TimeRange{time::RationalTime{0, sourceRate},
                           time::RationalTime::fromSeconds(ref->info.duration, sourceRate)};
}

}  // namespace

std::int32_t TimelineWidget::soundBlockTop(bool past) const {
    const model::Sequence* seq = sequence();
    const auto& metrics = layout_.metrics();
    if (seq == nullptr) {
        return metrics.rulerHeight;
    }
    // The bottom edge of everything above: the picture rows for the top of the
    // sound block, and every row for the empty space past the last of them.
    std::int32_t edge = metrics.rulerHeight;
    for (const ui::TimelineLayout::Row& row : layout_.rows(*seq)) {
        if (past || row.kind == model::TrackKind::Video) {
            edge = std::max(edge, row.top + row.height + metrics.trackGap);
        }
    }
    return edge;
}

std::optional<TimelineWidget::DropSpot> TimelineWidget::dropSpotFor(const MediaDrag& dragged,
                                                                    const QPoint& at) {
    const model::Sequence* seq = sequence();
    if (project_ == nullptr || commands_ == nullptr || seq == nullptr) {
        return std::nullopt;
    }
    // A title carries no media -- it generates its picture -- so what is being
    // dragged is a preset, and its length is the preset's rather than a file's.
    const TitlePreset* preset = dragged.isTitle() ? findTitlePreset(dragged.titlePreset) : nullptr;
    const model::MediaRef* ref = preset != nullptr ? nullptr : project_->findMedia(dragged.media);
    if (preset == nullptr && ref == nullptr) {
        return std::nullopt;
    }
    const auto& metrics = layout_.metrics();
    if (at.x() < metrics.headerWidth) {
        return std::nullopt;  // over the track headers, which are not a time
    }

    const time::Rational& rate = seq->frameRate();
    DropSpot spot;
    if (preset != nullptr) {
        spot.duration = defaultTitleLength(rate);
    } else {
        const auto sourceRange = draggedSourceRange(*project_, dragged, rate);
        if (!sourceRange) {
            return std::nullopt;
        }
        spot.duration = sourceRange->duration().rescaledTo(rate);
    }
    if (spot.duration.frames() <= 0) {
        return std::nullopt;
    }

    // Where the block of picture rows ends and the block of sound rows begins,
    // which is the line that decides what a drop between rows -- or past the
    // last one -- means. Measured off the rows rather than multiplied out of a
    // height: rows may be any height, and two of the same kind need not match.
    const std::int32_t audioTop = soundBlockTop();

    const auto row = layout_.rowAt(*seq, at.y());
    spot.at.kind =
        row ? row->kind : (at.y() < audioTop ? model::TrackKind::Video : model::TrackKind::Audio);
    // Sound has nowhere to be on a picture row: a file with no picture in it
    // would draw as a blank block and show nothing. A take with picture in it
    // may be aimed at either block, and brings both halves either way; where
    // the pointer is decides which half lands under it.
    //
    // A title is the mirror of the first case: it is picture and nothing else,
    // so a sound row is not somewhere it can go, whatever it was aimed at.
    if (preset != nullptr) {
        spot.at.kind = model::TrackKind::Video;
    } else if (ref->info.primaryVideo() == nullptr) {
        spot.at.kind = model::TrackKind::Audio;
    }

    spot.start = maybeSnap(layout_.timeForX(at.x(), rate), model::ClipId{});
    if (spot.start.frames() < 0) {
        spot.start = time::RationalTime{0, rate};
    }
    const time::TimeRange range{spot.start, spot.duration};

    // A band along the outer edge of the rows asks for a row of its own.
    //
    // Above the top picture row, and below the bottom sound row, there is
    // nothing else the gesture could mean -- and it is the only way to ask for
    // a new row at all when the one already there has room for the clip. The
    // band is at the top of the block rather than above it because there is no
    // "above": the ruler is there, and the rows start directly under it.
    constexpr std::int32_t kNewRowBand = 12;
    bool wantsNewRow = false;
    if (at.y() < metrics.rulerHeight + kNewRowBand &&
        (preset != nullptr || ref->info.primaryVideo() != nullptr)) {
        // Named as picture even where there are no picture rows yet, which is
        // the one case where the top of the block and the top of the sound
        // block are the same pixel.
        spot.at.kind = model::TrackKind::Video;
        wantsNewRow = true;
    } else if (spot.at.kind == model::TrackKind::Audio && !row) {
        wantsNewRow = true;  // past the last sound row, over nothing at all
    }

    // Otherwise the row under the pointer, unless the redirection above moved
    // the drop to the other block, in which case the first row of that block is
    // what it was nearest to.
    const auto& tracks =
        spot.at.kind == model::TrackKind::Video ? seq->videoTracks() : seq->audioTracks();
    model::TrackId wanted;
    if (wantsNewRow) {
        wanted = model::TrackId{};
    } else if (row && row->kind == spot.at.kind) {
        wanted = row->track;
    } else if (!tracks.empty()) {
        wanted = tracks.front().id();
    }
    spot.at = landingFor(spot.at.kind, wanted, range);

    // The other half of the take, on the first row of the other block -- A1 or
    // V1 if it is free there, and a row of its own if it is not. Not a
    // property of where the pointer is: the pointer chose which half lands
    // under it, and a take that has picture and sound arrives whole either
    // way. Dropping a take on a sound row used to bring in its sound alone,
    // which meant the same file behaved as two different files depending on
    // which row it was let go of over.
    //
    // A title has no other half at all: it is picture and nothing else.
    const bool wantsSound = ref != nullptr && spot.at.kind == model::TrackKind::Video &&
                            ref->info.primaryAudio() != nullptr;
    const bool wantsPicture = ref != nullptr && spot.at.kind == model::TrackKind::Audio &&
                              ref->info.primaryVideo() != nullptr;
    if (wantsSound || wantsPicture) {
        const model::TrackKind kind =
            wantsSound ? model::TrackKind::Audio : model::TrackKind::Video;
        const auto& partnerTracks =
            kind == model::TrackKind::Video ? seq->videoTracks() : seq->audioTracks();
        spot.partner = landingFor(
            kind, partnerTracks.empty() ? model::TrackId{} : partnerTracks.front().id(), range);
    }

    return spot;
}

TimelineWidget::DropSpot::Landing TimelineWidget::landingFor(model::TrackKind kind,
                                                             model::TrackId wanted,
                                                             const time::TimeRange& range) const {
    DropSpot::Landing landing;
    landing.kind = kind;
    const model::Sequence* seq = sequence();
    const model::Track* track =
        seq != nullptr && wanted.isValid() ? seq->findTrack(wanted) : nullptr;
    // A locked track refuses an edit as firmly as an occupied one, and means
    // the same thing here: not this row, then.
    landing.newTrack = track == nullptr || track->isLocked() || !track->clipsIn(range).empty();
    landing.track = landing.newTrack ? model::TrackId{} : wanted;
    return landing;
}

/// Where the ghost goes, worked out at paint time rather than kept in the spot.
///
/// The view can zoom or scroll between the pointer moving and the panel
/// repainting, and a rectangle worked out at the earlier moment would be drawn
/// against the later one -- a ghost sitting somewhere the clip is not going.
QRectF TimelineWidget::dropPreviewRect(const DropSpot& spot,
                                       const DropSpot::Landing& landing) const {
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return {};
    }
    const auto& metrics = layout_.metrics();
    const bool isVideo = landing.kind == model::TrackKind::Video;

    // On an existing row the ghost is the clip's own body. On a new one it is
    // the strip the new row will occupy: the top of the picture block, since
    // video stacks upward, or the empty space under the last sound row.
    const std::optional<ui::TimelineLayout::Row> placed =
        landing.newTrack ? std::nullopt : rowFor(landing.track);
    const double top =
        (placed ? placed->top : (isVideo ? metrics.rulerHeight : soundBlockTop(true))) + 2.0;
    const double height =
        (placed ? placed->height
                : (isVideo ? metrics.videoTrackHeight : metrics.audioTrackHeight)) -
        4.0;

    const double left = std::max<double>(layout_.xForTime(spot.start), metrics.headerWidth);
    const double right = layout_.xForTime(spot.start + spot.duration);
    return QRectF{left, top, std::max(2.0, right - left), std::max(2.0, height)};
}

/// What a row added now would be called: the next V or A number along.
QString TimelineWidget::nextTrackBadge(model::TrackKind kind) const {
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return {};
    }
    const std::size_t count =
        kind == model::TrackKind::Video ? seq->videoTracks().size() : seq->audioTracks().size();
    return QString{kind == model::TrackKind::Video ? "V%1" : "A%1"}.arg(count + 1);
}

void TimelineWidget::paintDropPreview(QPainter& painter) {
    if (!dropSpot_) {
        return;
    }
    const DropSpot& spot = *dropSpot_;
    painter.setRenderHint(QPainter::Antialiasing, true);

    // One ghost per clip the drop would make, so a file with sound in it shows
    // both rows it is about to occupy rather than only the one under the
    // pointer.
    const auto ghost = [this, &painter, &spot](const DropSpot::Landing& landing) {
        const QRectF body = dropPreviewRect(spot, landing);
        if (body.isEmpty()) {
            return;
        }
        const QColor accent =
            landing.kind == model::TrackKind::Audio ? theme::audio(400) : theme::accent(400);

        if (landing.newTrack) {
            // The whole lane, not just the clip: what is being previewed is a
            // row that does not exist yet, and a ghost floating inside a lane
            // that is already somebody else's would say the opposite of what
            // happens.
            const double headers = layout_.metrics().headerWidth;
            const QRectF lane{headers, body.top() - 2.0, width() - headers, body.height() + 4.0};
            QColor wash = accent;
            wash.setAlpha(30);
            painter.fillRect(lane, wash);
            // A solid bar along the edge the row opens at. The picture block
            // grows upward, so a new row's strip is drawn over the row
            // currently at the top of it -- the bar is what says "a lane opens
            // here" rather than "this is going onto that track".
            painter.fillRect(QRectF{headers, lane.top(), lane.width(), 3.0}, accent);

            // And a header for the row that does not exist yet, named as it
            // will be named, in the column where every other row is named. The
            // plainest way to say a channel is about to be created is to draw
            // the channel: a wash over a lane is a hint, "+ V3" is a promise.
            painter.save();
            const QRectF header{0.0, lane.top(), headers, lane.height()};
            painter.fillRect(header, kHeaderBackground);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen{accent, 1.0, Qt::DashLine});
            painter.drawRect(header.adjusted(0.5, 0.5, -0.5, -0.5));
            painter.setFont(badgeFont());
            painter.setPen(accent);
            painter.drawText(header.adjusted(10.0, 0.0, -6.0, 0.0),
                             Qt::AlignVCenter | Qt::AlignLeft,
                             QStringLiteral("+ ") + nextTrackBadge(landing.kind));
            painter.restore();
        }

        QColor fill = accent;
        fill.setAlpha(90);
        painter.setBrush(fill);
        painter.setPen(QPen{accent, landing.newTrack ? 2.0 : 1.0, Qt::DashLine});
        painter.drawRoundedRect(body, 5.0, 5.0);
    };

    ghost(spot.at);
    if (spot.partner) {
        ghost(*spot.partner);
    }
}

/// Where the clip being dragged would land.
///
/// Drawn in the same language as a file dragged in from the bin -- a dashed
/// ghost on the row it would join -- because it is the same question asked
/// about a clip that is already on the timeline. The clip itself stays where
/// it is until the button comes up, so what is on screen during the drag is
/// the cut as it stands beside the cut as it would be.
void TimelineWidget::paintMovePreview(QPainter& painter) {
    const model::Sequence* seq = sequence();
    if (!movePreview_.active || seq == nullptr) {
        return;
    }
    const model::Track* from = seq->findTrack(selectedTrack_);
    const model::Clip* clip = from != nullptr ? from->find(selected_) : nullptr;
    if (clip == nullptr) {
        return;
    }
    if (movePreview_.track == selectedTrack_ && movePreview_.delta.frames() == 0) {
        return;  // nothing would change; a ghost over the clip itself says nothing
    }

    painter.setRenderHint(QPainter::Antialiasing, true);
    const double headers = layout_.metrics().headerWidth;

    const auto ghost = [this, &painter, headers](model::TrackId trackId,
                                                 const time::RationalTime& start,
                                                 const time::RationalTime& duration) {
        const auto row = rowFor(trackId);
        if (!row || start.frames() < 0) {
            return;
        }
        const QColor accent =
            row->kind == model::TrackKind::Audio ? theme::audio(400) : theme::accent(400);
        const double left = layout_.xForTime(start);
        const double right = layout_.xForTime(start + duration);
        QRectF body{left, row->top + 2.0, std::max(1.0, right - left), row->height - 4.0};
        if (body.left() < headers) {
            body.setLeft(headers);
        }
        if (body.width() <= 0.0) {
            return;
        }
        QColor fill = accent;
        fill.setAlpha(90);
        painter.setBrush(fill);
        painter.setPen(QPen{accent, 1.0, Qt::DashLine});
        painter.drawRoundedRect(body, 5.0, 5.0);
    };

    if (selection_.size() > 1) {
        for (const edit::ClipRef& ref : selection_) {
            const model::Track* track = seq->findTrack(ref.track);
            const model::Clip* member = track != nullptr ? track->find(ref.clip) : nullptr;
            if (member != nullptr) {
                ghost(ref.track, member->start() + movePreview_.delta, member->duration());
            }
        }
        return;
    }

    ghost(movePreview_.track, movePreview_.start, clip->duration());

    // Whatever is linked to it shifts by the same amount on its own row, which
    // is what the move will do -- so the ghost says so before it happens.
    if (!clip->link.isValid()) {
        return;
    }
    for (const std::vector<model::Track>* tracks : {&seq->videoTracks(), &seq->audioTracks()}) {
        for (const model::Track& track : *tracks) {
            for (const model::Clip& other : track.clips()) {
                if (other.link == clip->link && other.id != clip->id) {
                    ghost(track.id(), other.start() + movePreview_.delta, other.duration());
                }
            }
        }
    }
}

void TimelineWidget::dragEnterEvent(QDragEnterEvent* event) {
    const auto dragged = decodeMediaDrag(event->mimeData());
    if (!dragged) {
        event->ignore();
        return;
    }
    dragged_ = *dragged;
    dropSpot_ = dropSpotFor(dragged_, event->position().toPoint());
    if (!dropSpot_) {
        event->ignore();
        return;
    }
    // Copy, not move: the bin keeps the file. Taking it out of the bin because
    // it was put on the timeline is not what anybody means by this gesture.
    event->setDropAction(Qt::CopyAction);
    event->acceptProposedAction();
    update();
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* event) {
    dropSpot_ = dropSpotFor(dragged_, event->position().toPoint());
    if (!dropSpot_) {
        clearGestureMarks();
        update();
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->acceptProposedAction();
    update();
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent* event) {
    dropSpot_.reset();
    clearGestureMarks();
    update();
    event->accept();
}

void TimelineWidget::dropEvent(QDropEvent* event) {
    dropSpot_.reset();
    const auto dragged = decodeMediaDrag(event->mimeData());
    if (!dragged || project_ == nullptr || commands_ == nullptr || sequence() == nullptr) {
        clearGestureMarks();
        update();
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();

    // One undo step for the whole drop, however many commands it takes: the
    // track it may have to make is part of putting the clip down rather than a
    // separate thing somebody asked for.
    const edit::CommandStack::Group step{*commands_};

    // The first file on an empty timeline decides its shape, exactly as it does
    // when the bin appends one. Before the drop is worked out rather than
    // after: conforming changes the frame rate the start time is counted in.
    if (const model::MediaRef* ref =
            dragged->isTitle() ? nullptr : project_->findMedia(dragged->media)) {
        const media::VideoStreamInfo* video = ref->info.primaryVideo();
        if (video != nullptr && sequence()->duration().frames() == 0) {
            if (auto conformed = edit::makeConformSequence(*project_, sequenceId_, video->frameRate,
                                                           video->width, video->height)) {
                commands_->execute(*project_, std::move(*conformed));
            }
        }
    }

    const auto spot = dropSpotFor(*dragged, event->position().toPoint());
    if (spot) {
        placeDropped(*dragged, *spot);
    }
    clearGestureMarks();
    update();
}

model::TrackId TimelineWidget::placeOne(const MediaDrag& dragged, const DropSpot& where,
                                        const DropSpot::Landing& landing, model::ClipId& placed) {
    const model::Sequence* seq = sequence();
    if (seq == nullptr || project_ == nullptr) {
        return {};
    }
    const TitlePreset* preset = dragged.isTitle() ? findTitlePreset(dragged.titlePreset) : nullptr;
    const model::MediaRef* ref = preset != nullptr ? nullptr : project_->findMedia(dragged.media);
    std::optional<time::TimeRange> sourceRange;
    if (preset == nullptr) {
        if (ref == nullptr) {
            return {};
        }
        sourceRange = draggedSourceRange(*project_, dragged, seq->frameRate());
        if (!sourceRange) {
            return {};
        }
    }

    model::TrackId target = landing.track;
    if (landing.newTrack) {
        addTrack(landing.kind);
        // The command replaced the sequence wholesale, so nothing read before
        // this line may be used after it. The new track is the last of its kind.
        seq = sequence();
        if (seq == nullptr) {
            return {};
        }
        const auto& tracks =
            landing.kind == model::TrackKind::Video ? seq->videoTracks() : seq->audioTracks();
        if (tracks.empty()) {
            return {};
        }
        target = tracks.back().id();
    }

    const time::TimeRange range{where.start, where.duration};

    // A title is added rather than overwritten in: `makeAddGraphic` is what
    // knows how to give a clip with no media a source range, and going through
    // it means a dragged title and one made from the menu are the same clip.
    if (preset != nullptr) {
        const model::Graphic graphic = graphicFor(*preset, seq->width(), seq->height());
        auto builtTitle = edit::makeAddGraphic(*project_, {sequenceId_, target}, graphic, range);
        if (!builtTitle) {
            return {};
        }
        commands_->execute(*project_, std::move(*builtTitle));
        const model::Sequence* after = sequence();
        const model::Track* landed = after != nullptr ? after->findTrack(target) : nullptr;
        if (landed == nullptr) {
            return {};
        }
        for (const model::Clip& made : landed->clips()) {
            if (made.start() == range.start() && made.graphic.kind == model::GraphicKind::Text) {
                placed = made.id;
                return target;
            }
        }
        return {};
    }

    const model::Subclip* subclip = project_->findSubclip(dragged.subclip);
    model::Clip clip;
    clip.id = project_->ids().next<model::ClipTag>();
    clip.source = dragged.media;
    clip.name = subclip != nullptr && !subclip->name.empty() ? subclip->name : ref->name;
    clip.sourceRange = *sourceRange;
    clip.timelineRange = range;

    auto built = edit::makeOverwrite(*project_, {sequenceId_, target}, clip);
    if (!built) {
        return {};
    }
    commands_->execute(*project_, std::move(*built));
    placed = clip.id;
    return target;
}

void TimelineWidget::placeDropped(const MediaDrag& dragged, const DropSpot& where) {
    if (project_ == nullptr || commands_ == nullptr || sequence() == nullptr) {
        return;
    }

    const edit::CommandStack::Group step{*commands_};

    // The half the pointer aimed at goes down first, so a take dropped on a
    // sound row lands where it was let go of and its picture follows.
    model::ClipId aimedClip;
    const model::TrackId aimedTrack = placeOne(dragged, where, where.at, aimedClip);
    if (!aimedTrack.isValid()) {
        return;
    }
    model::TrackId pictureTrack =
        where.at.kind == model::TrackKind::Video ? aimedTrack : model::TrackId{};
    model::ClipId pictureClip =
        where.at.kind == model::TrackKind::Video ? aimedClip : model::ClipId{};

    // The other half of the same take, on a row of its own, joined to it.
    //
    // Two clips rather than one because that is what the program can hear: the
    // audio graph mixes clips on audio tracks and nothing else, so a take that
    // arrived as a single clip on V1 played silently. Linked because picture
    // and sound that arrived together should stay together -- dragging one and
    // leaving the other is how a cut goes out of sync without anyone noticing.
    if (where.partner) {
        model::ClipId partnerClip;
        const model::TrackId partnerTrack = placeOne(dragged, where, *where.partner, partnerClip);
        if (partnerTrack.isValid()) {
            if (auto linked =
                    edit::makeLinkClips(*project_, sequenceId_,
                                        {{aimedTrack, aimedClip}, {partnerTrack, partnerClip}})) {
                commands_->execute(*project_, std::move(*linked));
            }
            if (where.partner->kind == model::TrackKind::Video) {
                pictureTrack = partnerTrack;
                pictureClip = partnerClip;
            }
        }
    }

    commands_->breakMerge();
    // Selected, the way a clip somebody has just put down is the one they are
    // about to move or trim. The picture where there is one, whichever row it
    // was aimed at: the picture is the half somebody points at, and the link
    // brings the other along anyway.
    if (pictureTrack.isValid()) {
        selectOnly(pictureTrack, pictureClip);
    } else {
        selectOnly(aimedTrack, aimedClip);
    }
    emit edited();
    emit viewChanged();
    update();
}

}  // namespace zaro::app
