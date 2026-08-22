#include "MaskOverlay.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <cmath>

#include "zaro/core/edit/Operations.h"

#include "ProgramMonitor.h"

namespace zaro::app {
namespace {

const QColor kOutline{255, 214, 102};
const QColor kPoint{255, 255, 255};
const QColor kHandle{150, 190, 255};
const QColor kShadow{20, 20, 24};

/// How near a click has to be, in widget pixels. Generous, because a handle is
/// a few pixels across and somebody aiming at one with a trackpad is not going
/// to land on the middle of it.
constexpr double kGrab = 9.0;

}  // namespace

MaskOverlay::MaskOverlay(ProgramMonitor* monitor, QWidget* parent)
    : QWidget{parent}, monitor_{monitor} {
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAutoFillBackground(false);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    hide();
}

void MaskOverlay::setTarget(model::Project* project, model::SequenceId sequence,
                            model::TrackId track, model::ClipId clip,
                            edit::CommandStack* commands) {
    project_ = project;
    sequenceId_ = sequence;
    trackId_ = track;
    clipId_ = clip;
    commands_ = commands;
    dragging_.reset();
    // Switching clips abandons a half-drawn path rather than moving it to the
    // new clip, which is not what anybody meant by clicking away.
    if (drawing_) {
        setDrawing(false);
    }
    // Hidden when there is nothing to edit, so it never swallows a click meant
    // for the picture underneath.
    setVisible(isEditing());
    update();
}

const model::Clip* MaskOverlay::clip() const {
    if (project_ == nullptr || !clipId_.isValid()) {
        return nullptr;
    }
    const model::Sequence* sequence = project_->findSequence(sequenceId_);
    const model::Track* track = sequence != nullptr ? sequence->findTrack(trackId_) : nullptr;
    return track != nullptr ? track->find(clipId_) : nullptr;
}

const model::Mask* MaskOverlay::mask() const {
    const model::Clip* found = clip();
    if (found == nullptr || found->mask.shape != model::MaskShape::Path ||
        !found->mask.path.isSet()) {
        return nullptr;
    }
    return &found->mask;
}

bool MaskOverlay::isEditing() const {
    return mask() != nullptr;
}

void MaskOverlay::setDrawing(bool drawing) {
    if (drawing == drawing_) {
        return;
    }
    drawing_ = drawing && clip() != nullptr;
    pending_.points.clear();
    shaping_ = false;
    dragging_.reset();
    // Visible while drawing even though there is no path yet: the pen has to
    // receive the clicks that make one.
    setVisible(isEditing() || drawing_);
    if (drawing_) {
        setFocus(Qt::OtherFocusReason);
    }
    emit drawingChanged(drawing_);
    update();
}

bool MaskOverlay::overFirstPoint(const QPointF& where) const {
    if (pending_.points.size() < 3) {
        // Closing early would leave a mask that encloses nothing, so the first
        // point is not a target until there is something to close.
        return false;
    }
    const model::MaskPoint& first = pending_.points.front();
    return QLineF(toWidget(first.x, first.y), where).length() <= kGrab;
}

void MaskOverlay::closePath() {
    const model::Clip* found = clip();
    if (found == nullptr || commands_ == nullptr || !pending_.isSet()) {
        return;
    }
    // The rest of the mask is kept: feather and invert are properties of the
    // mask rather than of the shape, and somebody who set them before drawing
    // did not ask for them back at their defaults.
    model::Mask drawn = found->mask;
    drawn.shape = model::MaskShape::Path;
    drawn.path = pending_;
    auto built = edit::makeSetMask(*project_, {sequenceId_, trackId_}, clipId_, drawn);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    setDrawing(false);
    emit edited();
    update();
}

QPointF MaskOverlay::toWidget(double x, double y) const {
    const QRectF picture = monitor_->pictureRect();
    const model::Sequence* sequence =
        project_ != nullptr ? project_->findSequence(sequenceId_) : nullptr;
    if (sequence == nullptr || sequence->width() <= 0) {
        return {};
    }
    const double scale = picture.width() / static_cast<double>(sequence->width());
    return {picture.center().x() + (x * scale), picture.center().y() + (y * scale)};
}

QPointF MaskOverlay::toFrame(const QPointF& widget) const {
    const QRectF picture = monitor_->pictureRect();
    const model::Sequence* sequence =
        project_ != nullptr ? project_->findSequence(sequenceId_) : nullptr;
    if (sequence == nullptr || sequence->width() <= 0 || picture.width() <= 0.0) {
        return {};
    }
    const double scale = static_cast<double>(sequence->width()) / picture.width();
    return {(widget.x() - picture.center().x()) * scale,
            (widget.y() - picture.center().y()) * scale};
}

std::optional<MaskOverlay::Hit> MaskOverlay::hitTest(const QPointF& where) const {
    const model::Mask* found = mask();
    if (found == nullptr) {
        return std::nullopt;
    }
    const auto& points = found->path.points;
    // Handles first: they sit away from their point, but a handle pulled in
    // close would otherwise be unreachable under the point that owns it.
    for (std::size_t i = 0; i < points.size(); ++i) {
        const model::MaskPoint& point = points[i];
        const std::pair<Part, QPointF> handles[] = {
            {Part::HandleIn, toWidget(point.x + point.inX, point.y + point.inY)},
            {Part::HandleOut, toWidget(point.x + point.outX, point.y + point.outY)}};
        const QPointF anchor = toWidget(point.x, point.y);
        for (const auto& [part, at] : handles) {
            if (QLineF(anchor, at).length() < 0.5) {
                continue;  // a corner has no handle drawn, so none to grab either
            }
            if (QLineF(at, where).length() <= kGrab) {
                return Hit{i, part};
            }
        }
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (QLineF(toWidget(points[i].x, points[i].y), where).length() <= kGrab) {
            return Hit{i, Part::Point};
        }
    }
    return std::nullopt;
}

void MaskOverlay::apply(const model::MaskPath& path, bool merge) {
    const model::Clip* found = clip();
    if (found == nullptr || commands_ == nullptr) {
        return;
    }
    model::Mask changed = found->mask;
    changed.path = path;
    auto built = edit::makeSetMask(*project_, {sequenceId_, trackId_}, clipId_, changed);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    if (!merge) {
        // A whole drag is one undo step, so the merge run is broken when the
        // mouse comes up rather than on every move.
        commands_->breakMerge();
    }
    emit edited();
    update();
}

void MaskOverlay::paintEvent(QPaintEvent* event) {
    static_cast<void>(event);
    if (drawing_) {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        // Only the pending path while the pen is out: the old one is about to
        // be replaced, and two outlines on the picture would be two things
        // that look equally editable when one of them is not.
        paintPending(painter);
        return;
    }
    const model::Mask* found = mask();
    if (found == nullptr) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // The outline, as the path itself rather than as straight lines between
    // points: an outline that ignored the handles would sit visibly off the
    // edge it is supposed to be showing.
    const auto& points = found->path.points;
    QPainterPath outline;
    outline.moveTo(toWidget(points.front().x, points.front().y));
    for (std::size_t i = 0; i < points.size(); ++i) {
        const model::MaskPoint& from = points[i];
        const model::MaskPoint& to = points[(i + 1) % points.size()];
        outline.cubicTo(toWidget(from.x + from.outX, from.y + from.outY),
                        toWidget(to.x + to.inX, to.y + to.inY), toWidget(to.x, to.y));
    }
    // Drawn twice: a dark line under a light one, so the outline is visible on
    // a white picture and on a black one without anybody choosing a colour for
    // the footage.
    painter.setPen(QPen(kShadow, 3.0));
    painter.drawPath(outline);
    painter.setPen(QPen(kOutline, 1.5));
    painter.drawPath(outline);

    for (const model::MaskPoint& point : points) {
        const QPointF at = toWidget(point.x, point.y);
        for (const QPointF& handle : {toWidget(point.x + point.inX, point.y + point.inY),
                                      toWidget(point.x + point.outX, point.y + point.outY)}) {
            if (QLineF(at, handle).length() < 0.5) {
                continue;  // a corner has no handle to show
            }
            painter.setPen(QPen(kHandle, 1.0));
            painter.drawLine(at, handle);
            painter.setBrush(kHandle);
            painter.drawEllipse(handle, 3.0, 3.0);
        }
        painter.setPen(QPen(kShadow, 1.0));
        painter.setBrush(kPoint);
        painter.drawRect(QRectF(at.x() - 3.5, at.y() - 3.5, 7.0, 7.0));
    }
}

void MaskOverlay::paintPending(QPainter& painter) const {
    const auto& points = pending_.points;
    if (points.empty()) {
        return;
    }
    QPainterPath drawn;
    drawn.moveTo(toWidget(points.front().x, points.front().y));
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const model::MaskPoint& from = points[i];
        const model::MaskPoint& to = points[i + 1];
        drawn.cubicTo(toWidget(from.x + from.outX, from.y + from.outY),
                      toWidget(to.x + to.inX, to.y + to.inY), toWidget(to.x, to.y));
    }
    painter.setPen(QPen(kShadow, 3.0));
    painter.drawPath(drawn);
    painter.setPen(QPen(kOutline, 1.5));
    painter.drawPath(drawn);

    // The segment that follows the pointer, dashed so it reads as not placed
    // yet rather than as part of the path.
    if (!cursor_.isNull()) {
        QPen chasing(kOutline, 1.0, Qt::DashLine);
        painter.setPen(chasing);
        const model::MaskPoint& last = points.back();
        painter.drawLine(toWidget(last.x, last.y), cursor_);
        if (points.size() >= 3) {
            painter.drawLine(cursor_, toWidget(points.front().x, points.front().y));
        }
    }

    for (std::size_t i = 0; i < points.size(); ++i) {
        const QPointF at = toWidget(points[i].x, points[i].y);
        painter.setPen(QPen(kShadow, 1.0));
        painter.setBrush(kPoint);
        painter.drawRect(QRectF(at.x() - 3.5, at.y() - 3.5, 7.0, 7.0));
    }
    // A ring on the first point once closing is possible, so the gesture that
    // ends the path is visible rather than something you have to know.
    if (points.size() >= 3) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(kOutline, 1.5));
        const QPointF first = toWidget(points.front().x, points.front().y);
        painter.drawEllipse(first, kGrab, kGrab);
    }
}

void MaskOverlay::mousePressEvent(QMouseEvent* event) {
    if (drawing_) {
        const QPointF where = event->position();
        if (overFirstPoint(where)) {
            closePath();
            return;
        }
        const QPointF frame = toFrame(where);
        model::MaskPoint point;
        point.x = frame.x();
        point.y = frame.y();
        pending_.points.push_back(point);
        // Held down: dragging away from the point being placed pulls a handle
        // out of it, which is the difference between a corner and a curve.
        shaping_ = true;
        cursor_ = where;
        update();
        return;
    }
    const model::Mask* found = mask();
    if (found == nullptr) {
        event->ignore();
        return;
    }
    const auto hit = hitTest(event->position());
    if (!hit) {
        // Nothing under the pointer. Passed on rather than swallowed: a click
        // on the picture beside the mask should still reach whatever is
        // underneath.
        event->ignore();
        return;
    }
    if (event->modifiers().testFlag(Qt::AltModifier) && hit->part == Part::Point) {
        // Alt-click deletes, the same gesture the keyframe lane uses. Refused
        // at three points, because fewer than three enclose nothing and a mask
        // that vanished would look like the delete having gone wrong.
        if (found->path.points.size() <= 3) {
            return;
        }
        model::MaskPath path = found->path;
        path.points.erase(path.points.begin() + static_cast<std::ptrdiff_t>(hit->index));
        apply(path, false);
        return;
    }
    dragging_ = hit;
}

void MaskOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (drawing_) {
        cursor_ = event->position();
        if (shaping_ && !pending_.points.empty()) {
            model::MaskPoint& point = pending_.points.back();
            const QPointF frame = toFrame(cursor_);
            point.outX = frame.x() - point.x;
            point.outY = frame.y() - point.y;
            // The incoming handle mirrors the outgoing one, so the curve runs
            // smoothly through the point. Breaking that symmetry is what
            // dragging a handle afterwards is for.
            point.inX = -point.outX;
            point.inY = -point.outY;
        }
        update();
        return;
    }
    if (!dragging_) {
        return;
    }
    const model::Mask* found = mask();
    if (found == nullptr || dragging_->index >= found->path.points.size()) {
        dragging_.reset();
        return;
    }
    const QPointF where = toFrame(event->position());
    model::MaskPath path = found->path;
    model::MaskPoint& point = path.points[dragging_->index];
    switch (dragging_->part) {
        case Part::Point:
            // The handles travel with their point: they are offsets from it,
            // and a point that moved out from under them would change the
            // curve as a side effect of being moved.
            point.x = where.x();
            point.y = where.y();
            break;
        case Part::HandleIn:
            point.inX = where.x() - point.x;
            point.inY = where.y() - point.y;
            break;
        case Part::HandleOut:
            point.outX = where.x() - point.x;
            point.outY = where.y() - point.y;
            break;
    }
    apply(path, true);
}

void MaskOverlay::mouseReleaseEvent(QMouseEvent* event) {
    static_cast<void>(event);
    if (drawing_) {
        shaping_ = false;
        return;
    }
    if (dragging_ && commands_ != nullptr) {
        commands_->breakMerge();
    }
    dragging_.reset();
}

void MaskOverlay::keyPressEvent(QKeyEvent* event) {
    if (!drawing_) {
        event->ignore();
        return;
    }
    switch (event->key()) {
        case Qt::Key_Escape:
            // Abandoned, not committed: nothing was written to the clip, so
            // there is nothing to undo either.
            setDrawing(false);
            return;
        case Qt::Key_Backspace:
        case Qt::Key_Delete:
            if (!pending_.points.empty()) {
                pending_.points.pop_back();
                update();
            }
            return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            closePath();
            return;
        default:
            event->ignore();
            return;
    }
}

}  // namespace zaro::app
