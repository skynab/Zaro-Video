#include "TimelineWidget.h"

#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/edit/Snapping.h"
#include "zaro/core/time/Timecode.h"

namespace zaro::app {
namespace {

const QColor kBackground{28, 28, 32};
const QColor kRulerBackground{40, 40, 46};
const QColor kHeaderBackground{34, 34, 40};
const QColor kTrackBackground{24, 24, 28};
const QColor kGridLine{58, 58, 66};
const QColor kVideoClip{62, 96, 148};
const QColor kAudioClip{58, 122, 96};
const QColor kSelectedOutline{255, 196, 92};
const QColor kPlayhead{236, 92, 82};
const QColor kText{224, 224, 230};
const QColor kDimText{150, 150, 160};
const QColor kWaveform{188, 236, 210};

}  // namespace

TimelineWidget::TimelineWidget(QWidget* parent) : QWidget{parent} {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumHeight(180);
    setAutoFillBackground(false);
}

void TimelineWidget::setProject(model::Project* project, model::SequenceId sequence,
                                edit::CommandStack* commands) {
    project_ = project;
    sequenceId_ = sequence;
    commands_ = commands;
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
    paintPlayhead(painter);
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

        const time::Timecode code = time::timecodeFromFrames(frame, rate, dropFrame);
        const QString label = QString::fromStdString(code.toString());
        painter.setPen(kDimText);
        painter.drawText(QPointF(x + 4, fontMetrics.ascent() + 3), label);
    }

    painter.setPen(kGridLine);
    painter.drawLine(0, metrics.rulerHeight, width(), metrics.rulerHeight);
}

void TimelineWidget::paintTracks(QPainter& painter) {
    const model::Sequence& seq = *sequence();
    const auto& metrics = layout_.metrics();

    for (const ui::TimelineLayout::Row& row : layout_.rows(seq)) {
        const QRect lane(metrics.headerWidth, row.top, width() - metrics.headerWidth, row.height);
        painter.fillRect(lane, kTrackBackground);
        paintClips(painter, row);

        const QRect header(0, row.top, metrics.headerWidth, row.height);
        painter.fillRect(header, kHeaderBackground);
        painter.setPen(kGridLine);
        painter.drawRect(header.adjusted(0, 0, -1, -1));

        const model::Track* track = seq.findTrack(row.track);
        if (track == nullptr) {
            continue;
        }
        painter.setPen(track->isMuted() ? kDimText : kText);
        painter.drawText(header.adjusted(10, 0, -10, 0), Qt::AlignVCenter | Qt::AlignLeft,
                         QString::fromStdString(track->name()));

        QStringList flags;
        if (track->isMuted()) {
            flags << "M";
        }
        if (track->isLocked()) {
            flags << "L";
        }
        if (!flags.isEmpty()) {
            painter.setPen(kSelectedOutline);
            painter.drawText(header.adjusted(0, 0, -10, 0), Qt::AlignVCenter | Qt::AlignRight,
                             flags.join(' '));
        }
    }
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

        const QColor base = row.kind == model::TrackKind::Video ? kVideoClip : kAudioClip;
        painter.fillRect(body, clip->enabled ? base : base.darker(180));

        if (clip->id == selected_) {
            painter.setPen(QPen(kSelectedOutline, 2));
            painter.drawRect(body.adjusted(1, 1, -1, -1));
        } else {
            painter.setPen(base.lighter(135));
            painter.drawRect(body.adjusted(0.5, 0.5, -0.5, -0.5));
        }

        if (row.kind == model::TrackKind::Audio) {
            paintWaveform(painter, *clip, body);
        }

        if (body.width() > 28.0) {
            painter.setPen(kText);
            painter.drawText(body.adjusted(6, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft,
                             QString::fromStdString(clip->name));
        }
    }
}

void TimelineWidget::setWaveform(model::MediaRefId media,
                                 std::shared_ptr<const media::Waveform> waveform) {
    waveforms_[media.value()] = std::move(waveform);
    update();
}

void TimelineWidget::paintWaveform(QPainter& painter, const model::Clip& clip, const QRectF& body) {
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

    const double midY = body.center().y();
    const double halfHeight = body.height() * 0.42;
    painter.setPen(kWaveform);

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
        const time::RationalTime sourceTime = clip.sourceTimeAt(timelineTime);
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

void TimelineWidget::paintPlayhead(QPainter& painter) {
    const auto& metrics = layout_.metrics();
    const double x = layout_.xForTime(playhead_);
    if (x < metrics.headerWidth) {
        return;
    }
    painter.setPen(QPen(kPlayhead, 1.5));
    painter.drawLine(QPointF(x, 0), QPointF(x, height()));

    // A head on the playhead, so it can be found and grabbed in the ruler.
    QPolygonF head;
    head << QPointF(x - 6, 0) << QPointF(x + 6, 0) << QPointF(x, 9);
    painter.setBrush(kPlayhead);
    painter.setPen(Qt::NoPen);
    painter.drawPolygon(head);
    painter.setBrush(Qt::NoBrush);
}

// --- Input ------------------------------------------------------------------

time::RationalTime TimelineWidget::maybeSnap(const time::RationalTime& t,
                                             model::ClipId ignoring) const {
    const model::Sequence* seq = sequence();
    if (seq == nullptr || !snapEnabled_) {
        return t;
    }
    // A fixed pixel radius becomes a shrinking time radius as you zoom in,
    // which is what makes snapping feel helpful rather than obstructive.
    const double thresholdSeconds = 10.0 / layout_.metrics().pixelsPerSecond;
    const auto threshold = time::RationalTime::fromSeconds(
        time::Rational::approximate(thresholdSeconds), seq->frameRate());

    const auto result = edit::snapTime(*seq, t, threshold, ignoring, &playhead_);
    return result.time;
}

void TimelineWidget::scrubTo(int x) {
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    auto position = layout_.timeForX(x, seq->frameRate());
    position = maybeSnap(position, {});
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
    setFocus();
    const int x = static_cast<int>(event->position().x());
    const int y = static_cast<int>(event->position().y());

    if (layout_.isInRuler(x, y)) {
        drag_ = Drag::Scrub;
        scrubTo(x);
        return;
    }
    if (layout_.isInHeaders(x)) {
        return;
    }

    const auto hit = layout_.hitTest(*seq, x, y);
    if (!hit) {
        selected_ = {};
        selectedTrack_ = {};
        // Clicking empty timeline still moves the playhead: it is the most
        // common thing to want there.
        drag_ = Drag::Scrub;
        scrubTo(x);
        return;
    }

    selected_ = hit->clip;
    selectedTrack_ = hit->track;
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

void TimelineWidget::mouseMoveEvent(QMouseEvent* event) {
    const int x = static_cast<int>(event->position().x());
    const int y = static_cast<int>(event->position().y());

    if (drag_ == Drag::Scrub) {
        scrubTo(x);
        return;
    }
    if (drag_ == Drag::MoveClip) {
        updateDrag(x);
        return;
    }
    if (drag_ == Drag::TrimIn || drag_ == Drag::TrimOut) {
        updateTrim(x);
        return;
    }

    // A cursor that tells you what a click will do, before you make it.
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    if (const auto hit = layout_.hitTest(*seq, x, y)) {
        setCursor(hit->part == ui::TimelineLayout::Part::Body ? Qt::OpenHandCursor
                                                              : Qt::SizeHorCursor);
    } else {
        unsetCursor();
    }
}

void TimelineWidget::updateDrag(int x) {
    model::Sequence* seq = project_->findSequence(sequenceId_);
    if (seq == nullptr || !selected_.isValid() || commands_ == nullptr) {
        return;
    }
    auto start = layout_.timeForX(x, seq->frameRate()) - grabOffset_;
    start = maybeSnap(start, selected_);
    if (start.frames() < 0) {
        start = time::RationalTime{0, seq->frameRate()};
    }

    // Each move is its own command, and they coalesce: the merge key is the
    // clip, so a whole drag collapses into one undo step rather than several
    // hundred.
    auto built =
        edit::makeMove(*project_, {sequenceId_, selectedTrack_}, selected_, selectedTrack_, start);
    if (!built) {
        return;  // the move is not legal from here; leave the clip where it was
    }
    commands_->execute(*project_, std::move(*built));
    emit edited();
    update();
}

void TimelineWidget::finishDrag() {
    if (drag_ != Drag::None && drag_ != Drag::Scrub && commands_ != nullptr) {
        // Close the merge group, so the next gesture is a separate undo step.
        commands_->breakMerge();
    }
    drag_ = Drag::None;
    rippleTrim_ = false;
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* /*event*/) {
    finishDrag();
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
    } else {
        // Scroll by a fraction of the visible span, so the feel is the same at
        // every zoom level.
        const time::TimeRange visible = layout_.visibleRange(seq->frameRate());
        const std::int64_t delta =
            static_cast<std::int64_t>(-steps * visible.duration().frames() / 6.0);
        layout_.setScroll(layout_.scroll().rescaledTo(seq->frameRate()) +
                          time::RationalTime{delta, seq->frameRate()});
    }
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

void TimelineWidget::removeSelected(bool ripple) {
    if (project_ == nullptr || commands_ == nullptr || !selected_.isValid()) {
        return;
    }
    const edit::EditTarget target{sequenceId_, selectedTrack_};
    auto built = ripple ? edit::makeExtract(*project_, target, selected_)
                        : edit::makeLift(*project_, target, selected_);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    selected_ = {};
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
        case Qt::Key_Backspace:
        case Qt::Key_Delete:
            // Shift closes the gap, matching the lift/extract pair.
            removeSelected(event->modifiers().testFlag(Qt::ShiftModifier));
            return;
        case Qt::Key_S:
            snapEnabled_ = !snapEnabled_;
            return;
        case Qt::Key_Z:
            if (event->modifiers().testFlag(Qt::ControlModifier) ||
                event->modifiers().testFlag(Qt::MetaModifier)) {
                if (event->modifiers().testFlag(Qt::ShiftModifier)) {
                    commands_->redo(*project_);
                } else {
                    commands_->undo(*project_);
                }
                selected_ = {};
                emit edited();
                update();
                return;
            }
            break;
        case Qt::Key_Equal:
        case Qt::Key_Plus:
            layout_.zoomBy(1.4, width() / 2.0, seq->frameRate());
            update();
            return;
        case Qt::Key_Minus:
            layout_.zoomBy(1.0 / 1.4, width() / 2.0, seq->frameRate());
            update();
            return;
        case Qt::Key_Backslash:
            zoomToFit();
            return;
        default:
            break;
    }
    QWidget::keyPressEvent(event);
}

}  // namespace zaro::app
