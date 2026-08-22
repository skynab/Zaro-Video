#include "MaskOverlay.h"

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

void MaskOverlay::mousePressEvent(QMouseEvent* event) {
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
    if (dragging_ && commands_ != nullptr) {
        commands_->breakMerge();
    }
    dragging_.reset();
}

}  // namespace zaro::app
