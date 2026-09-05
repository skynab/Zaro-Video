#include "TitleOverlay.h"

#include <QApplication>
#include <QFont>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QTextOption>
#include <algorithm>
#include <cmath>

#include "zaro/core/edit/Operations.h"

#include "ProgramMonitor.h"

namespace zaro::app {
namespace {

const QColor kBox{150, 190, 255};
const QColor kHandle{255, 255, 255};
const QColor kShadow{20, 20, 24};
const QColor kGuide{255, 214, 102};
/// The ring round a box that is open for typing.
const QColor kTyping{150, 190, 255};

/// How near a corner counts as grabbing it, in widget pixels. Generous: a
/// handle is a few pixels across and nobody aims at the middle of one.
constexpr double kGrab = 10.0;

/// How near an edge has to be to latch, in widget pixels. Fixed in pixels
/// rather than in frame coordinates so that snapping feels the same however
/// large the monitor is.
constexpr double kSnap = 8.0;

/// The title-safe box every broadcaster asks for: 80% of the frame, centred.
constexpr double kTitleSafe = 0.8;

/// A box may be dragged small but not to nothing: a title with no box has
/// nowhere to lay its text out, and no handle left to grab.
constexpr double kLeastBox = 16.0;

}  // namespace

TitleOverlay::TitleOverlay(ProgramMonitor* monitor, QWidget* parent)
    : QWidget{parent}, monitor_{monitor} {
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAutoFillBackground(false);
    setMouseTracking(true);
    hide();
}

void TitleOverlay::setTarget(model::Project* project, model::SequenceId sequence,
                             model::TrackId track, model::ClipId clip,
                             edit::CommandStack* commands) {
    // Before anything is repointed: closing writes what was typed, and it has
    // to be written to the clip it was typed into rather than to the new one.
    if (isTyping()) {
        endTyping();
    }
    project_ = project;
    sequenceId_ = sequence;
    trackId_ = track;
    clipId_ = clip;
    commands_ = commands;
    dragging_ = Part::None;
    pending_ = Part::None;
    snappedX_.reset();
    snappedY_.reset();
    setVisible(isEditing());
    update();
}

const model::Clip* TitleOverlay::clip() const {
    if (project_ == nullptr || !clipId_.isValid()) {
        return nullptr;
    }
    const model::Sequence* sequence = project_->findSequence(sequenceId_);
    const model::Track* track = sequence != nullptr ? sequence->findTrack(trackId_) : nullptr;
    return track != nullptr ? track->find(clipId_) : nullptr;
}

const model::Graphic* TitleOverlay::graphic() const {
    const model::Clip* found = clip();
    if (found == nullptr || found->graphic.kind != model::GraphicKind::Text) {
        return nullptr;
    }
    return &found->graphic;
}

bool TitleOverlay::isEditing() const {
    return graphic() != nullptr;
}

bool TitleOverlay::isTyping() const {
    return typing_ && editor_ != nullptr;
}

void TitleOverlay::beginTyping() {
    const model::Graphic* found = graphic();
    if (found == nullptr || isTyping()) {
        return;
    }
    if (editor_ == nullptr) {
        editor_ = new QPlainTextEdit(this);
        // No frame and no scrollbars: the box drawn round it is the frame, and
        // a scrollbar inside a title box is chrome sitting on the picture. Text
        // longer than the box is what the box being resizable is for.
        editor_->setFrameShape(QFrame::NoFrame);
        editor_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        editor_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        editor_->installEventFilter(this);
        // Live, so the picture under the box keeps up with what is being
        // typed. Every keystroke is a command that coalesces on the clip's
        // merge key, the same way a drag does -- a typing pass is one undo
        // step, not one per letter.
        connect(editor_, &QPlainTextEdit::textChanged, this, [this] {
            const model::Graphic* current = graphic();
            if (fillingEditor_ || current == nullptr) {
                return;
            }
            model::Graphic typed = *current;
            typed.text = editor_->toPlainText().toStdString();
            if (typed != *current) {
                apply(typed);
            }
        });
    }

    // Broken first, so the pass cannot merge into whatever came before it -- a
    // drag of the box, most likely. Without this, unwinding the pass would take
    // the drag with it.
    if (commands_ != nullptr) {
        commands_->breakMerge();
    }
    stepsBefore_ = commands_ != nullptr ? commands_->position() : 0;
    fillingEditor_ = true;
    editor_->setPlainText(QString::fromStdString(found->text));
    fillingEditor_ = false;
    layOutEditor();
    typing_ = true;
    editor_->show();
    editor_->setFocus(Qt::MouseFocusReason);
    editor_->selectAll();
    update();
}

void TitleOverlay::endTyping() {
    if (!isTyping()) {
        return;
    }
    typing_ = false;
    editor_->hide();
    if (commands_ != nullptr) {
        // Close the merge group: the next thing anybody does is a separate undo
        // step, so one Ctrl+Z takes back the whole typing pass and no more.
        commands_->breakMerge();
    }
    update();
}

void TitleOverlay::abandonTyping() {
    if (!isTyping()) {
        return;
    }
    // Unwound rather than typed back. Writing the old text again would restore
    // the words and leave a command on the stack that does nothing -- so the
    // next Ctrl+Z would appear to do nothing too, which is the behaviour that
    // makes people stop trusting undo. Everything the pass added comes off.
    if (commands_ != nullptr && project_ != nullptr) {
        while (commands_->position() > stepsBefore_ && commands_->undo(*project_)) {
        }
        emit edited();
    }
    endTyping();
}

void TitleOverlay::layOutEditor() {
    const model::Graphic* found = graphic();
    if (editor_ == nullptr || found == nullptr) {
        return;
    }
    const QRectF box = boxRect();
    if (box.isEmpty()) {
        return;
    }
    editor_->setGeometry(box.toAlignedRect());

    // The face the title is drawn in, as near as a widget can get to it. The
    // rasteriser reads `pointSize` as pixels of the output frame, so it is
    // scaled by however much of the frame the monitor is showing -- otherwise
    // what is typed is the right words at the wrong size, and the box it has to
    // fit in is the one on screen.
    QFont face = editor_->font();
    if (!found->family.empty()) {
        face.setFamily(QString::fromStdString(found->family));
    }
    face.setBold(found->bold);
    face.setItalic(found->italic);
    const model::Sequence* sequence =
        project_ != nullptr ? project_->findSequence(sequenceId_) : nullptr;
    const QRectF picture = monitor_->pictureRect();
    double pixels = found->pointSize;
    if (sequence != nullptr && sequence->width() > 0 && picture.width() > 0.0) {
        pixels = found->pointSize * picture.width() / static_cast<double>(sequence->width());
    }
    face.setPixelSize(std::max(6, static_cast<int>(std::lround(pixels))));
    editor_->setFont(face);

    QTextOption option = editor_->document()->defaultTextOption();
    option.setAlignment(found->alignment < 0   ? Qt::AlignLeft
                        : found->alignment > 0 ? Qt::AlignRight
                                               : Qt::AlignHCenter);
    editor_->document()->setDefaultTextOption(option);

    // The title's own ink, over a wash dark enough to read it against whatever
    // the picture happens to be doing under the box. Not the title's own
    // background, which is usually nothing at all: this is the editor saying it
    // is open, and it goes away with it.
    const QColor ink =
        QColor::fromRgbF(static_cast<float>(found->red), static_cast<float>(found->green),
                         static_cast<float>(found->blue));
    editor_->setStyleSheet(QString("background: rgba(12, 14, 22, 190); color: %1; "
                                   "selection-background-color: rgba(150, 190, 255, 110);")
                               .arg(ink.name(QColor::HexRgb)));
}

bool TitleOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (watched != editor_) {
        return QWidget::eventFilter(watched, event);
    }
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            abandonTyping();
            return true;
        }
        // Return finishes; Shift+Return is the line break, because a title on
        // two lines is the ordinary case and a modifier to end typing would be
        // the thing nobody guesses.
        const bool plainReturn = (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
                                 (key->modifiers() & Qt::ShiftModifier) == 0;
        if (plainReturn) {
            endTyping();
            return true;
        }
    }
    // Clicking anywhere else is finishing: the click is meant for whatever it
    // landed on, and leaving the editor open over the picture would swallow it.
    if (event->type() == QEvent::FocusOut) {
        endTyping();
    }
    return QWidget::eventFilter(watched, event);
}

QPointF TitleOverlay::toWidget(double x, double y) const {
    const QRectF picture = monitor_->pictureRect();
    const model::Sequence* sequence =
        project_ != nullptr ? project_->findSequence(sequenceId_) : nullptr;
    if (sequence == nullptr || sequence->width() <= 0) {
        return {};
    }
    const double scale = picture.width() / static_cast<double>(sequence->width());
    return {picture.center().x() + (x * scale), picture.center().y() + (y * scale)};
}

QPointF TitleOverlay::toFrame(const QPointF& widget) const {
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

QRectF TitleOverlay::boxRect() const {
    const model::Graphic* found = graphic();
    if (found == nullptr) {
        return {};
    }
    const QPointF topLeft =
        toWidget(found->centreX - found->width * 0.5, found->centreY - found->height * 0.5);
    const QPointF bottomRight =
        toWidget(found->centreX + found->width * 0.5, found->centreY + found->height * 0.5);
    return QRectF{topLeft, bottomRight}.normalized();
}

TitleOverlay::Part TitleOverlay::partAt(const QPointF& where) const {
    const QRectF box = boxRect();
    if (box.isEmpty()) {
        return Part::None;
    }
    const auto near = [&where](const QPointF& corner) {
        return QLineF{corner, where}.length() <= kGrab;
    };
    if (near(box.topLeft())) {
        return Part::TopLeft;
    }
    if (near(box.topRight())) {
        return Part::TopRight;
    }
    if (near(box.bottomLeft())) {
        return Part::BottomLeft;
    }
    if (near(box.bottomRight())) {
        return Part::BottomRight;
    }
    return box.contains(where) ? Part::Body : Part::None;
}

double TitleOverlay::snapX(double x, bool allow) {
    snappedX_.reset();
    const model::Sequence* sequence =
        project_ != nullptr ? project_->findSequence(sequenceId_) : nullptr;
    if (!allow || sequence == nullptr) {
        return x;
    }
    const QRectF picture = monitor_->pictureRect();
    const auto width = static_cast<double>(sequence->width());
    if (picture.width() <= 0.0 || width <= 0.0) {
        return x;
    }
    // The threshold is in widget pixels, so it is converted rather than
    // guessed: eight pixels on screen is eight pixels of aim whatever the
    // monitor is showing.
    const double threshold = kSnap * width / picture.width();
    for (const double candidate : {0.0, -width * kTitleSafe * 0.5, width * kTitleSafe * 0.5}) {
        if (std::abs(x - candidate) <= threshold) {
            snappedX_ = candidate;
            return candidate;
        }
    }
    return x;
}

double TitleOverlay::snapY(double y, bool allow) {
    snappedY_.reset();
    const model::Sequence* sequence =
        project_ != nullptr ? project_->findSequence(sequenceId_) : nullptr;
    if (!allow || sequence == nullptr) {
        return y;
    }
    const QRectF picture = monitor_->pictureRect();
    const auto height = static_cast<double>(sequence->height());
    if (picture.height() <= 0.0 || height <= 0.0) {
        return y;
    }
    const double threshold = kSnap * height / picture.height();
    for (const double candidate : {0.0, -height * kTitleSafe * 0.5, height * kTitleSafe * 0.5}) {
        if (std::abs(y - candidate) <= threshold) {
            snappedY_ = candidate;
            return candidate;
        }
    }
    return y;
}

void TitleOverlay::apply(const model::Graphic& graphic) {
    if (project_ == nullptr || commands_ == nullptr || !clipId_.isValid()) {
        return;
    }
    auto built = edit::makeSetGraphic(*project_, {sequenceId_, trackId_}, clipId_, graphic);
    if (!built) {
        return;
    }
    // Each move is its own command and they coalesce on the clip's merge key,
    // so a whole drag collapses into one undo step rather than several hundred.
    commands_->execute(*project_, std::move(*built));
    emit edited();
    update();
}

void TitleOverlay::mousePressEvent(QMouseEvent* event) {
    const model::Graphic* found = graphic();
    // A press while typing is somebody putting the caret somewhere, or leaving:
    // either way it is the editor's, and dragging the box out from under a
    // caret is not what the press meant.
    if (isTyping()) {
        event->ignore();
        return;
    }
    if (found == nullptr || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    const Part part = partAt(event->position());
    if (part == Part::None) {
        // Not on the box: the click belongs to whatever is under the overlay.
        event->ignore();
        return;
    }
    pending_ = part;
    grabbedAt_ = event->position();
    startedFrom_ = *found;
    stepsAtPress_ = commands_ != nullptr ? commands_->position() : 0;
    event->accept();
}

void TitleOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (isTyping()) {
        event->ignore();
        return;
    }
    const QPointF fromPress = event->position() - grabbedAt_;
    const double travelled =
        std::sqrt(fromPress.x() * fromPress.x() + fromPress.y() * fromPress.y());
    if (dragging_ == Part::None && pending_ != Part::None &&
        travelled >= static_cast<double>(QApplication::startDragDistance())) {
        // Far enough to be a drag rather than an unsteady click.
        dragging_ = pending_;
        pending_ = Part::None;
    }

    if (dragging_ == Part::None) {
        // Only the cursor changes: a box that is not being dragged still says
        // where its handles are.
        switch (partAt(event->position())) {
            case Part::TopLeft:
            case Part::BottomRight:
                setCursor(Qt::SizeFDiagCursor);
                break;
            case Part::TopRight:
            case Part::BottomLeft:
                setCursor(Qt::SizeBDiagCursor);
                break;
            case Part::Body:
                setCursor(Qt::SizeAllCursor);
                break;
            case Part::None:
                unsetCursor();
                break;
        }
        event->ignore();
        return;
    }

    const QPointF from = toFrame(grabbedAt_);
    const QPointF to = toFrame(event->position());
    const double dx = to.x() - from.x();
    const double dy = to.y() - from.y();
    const bool snapping = (event->modifiers() & Qt::AltModifier) == 0;

    model::Graphic moved = startedFrom_;
    if (dragging_ == Part::Body) {
        moved.centreX = snapX(startedFrom_.centreX + dx, snapping);
        moved.centreY = snapY(startedFrom_.centreY + dy, snapping);
    } else {
        // A corner moves; the one opposite it stays put. That is what makes a
        // resize feel like dragging the corner rather than scaling the box
        // about its middle.
        const bool left = dragging_ == Part::TopLeft || dragging_ == Part::BottomLeft;
        const bool top = dragging_ == Part::TopLeft || dragging_ == Part::TopRight;
        const double fixedX =
            startedFrom_.centreX + (left ? startedFrom_.width * 0.5 : -startedFrom_.width * 0.5);
        const double fixedY =
            startedFrom_.centreY + (top ? startedFrom_.height * 0.5 : -startedFrom_.height * 0.5);
        const double movedX =
            snapX(startedFrom_.centreX +
                      (left ? -startedFrom_.width * 0.5 : startedFrom_.width * 0.5) + dx,
                  snapping);
        const double movedY =
            snapY(startedFrom_.centreY +
                      (top ? -startedFrom_.height * 0.5 : startedFrom_.height * 0.5) + dy,
                  snapping);
        moved.width = std::max(kLeastBox, std::abs(movedX - fixedX));
        moved.height = std::max(kLeastBox, std::abs(movedY - fixedY));
        moved.centreX = fixedX + (left ? -moved.width * 0.5 : moved.width * 0.5);
        moved.centreY = fixedY + (top ? -moved.height * 0.5 : moved.height * 0.5);
    }

    if (moved != startedFrom_) {
        apply(moved);
    }
    event->accept();
}

void TitleOverlay::mouseReleaseEvent(QMouseEvent* event) {
    if (dragging_ == Part::None) {
        // A press that never became a drag. Nothing was written, so there is
        // nothing to close off -- but the press is spent either way.
        pending_ = Part::None;
        event->ignore();
        return;
    }
    pending_ = Part::None;
    dragging_ = Part::None;
    snappedX_.reset();
    snappedY_.reset();
    if (commands_ != nullptr) {
        // Close the merge group, so the next gesture is a separate undo step.
        commands_->breakMerge();
    }
    update();
    event->accept();
}

void TitleOverlay::mouseDoubleClickEvent(QMouseEvent* event) {
    if (graphic() == nullptr || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    if (partAt(event->position()) == Part::None) {
        // Outside the box: not this title's double-click.
        event->ignore();
        return;
    }
    // The press that opened this double-click started a drag. Nothing moved --
    // a double-click is two clicks in the same place -- but the gesture is
    // still open, and leaving it open means the next pointer move resizes the
    // box somebody is trying to type into.
    dragging_ = Part::None;
    pending_ = Part::None;
    snappedX_.reset();
    snappedY_.reset();
    // Whatever that press managed to do before the second click identified the
    // gesture, undone. A threshold keeps a steady hand from starting a drag at
    // all, but a hand that drifts far enough is indistinguishable from somebody
    // beginning to drag -- until the second click arrives and says it was not.
    if (commands_ != nullptr && project_ != nullptr) {
        while (commands_->position() > stepsAtPress_ && commands_->undo(*project_)) {
        }
        emit edited();
    }
    beginTyping();
    event->accept();
}

void TitleOverlay::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (isTyping()) {
        layOutEditor();
    }
}

void TitleOverlay::paintEvent(QPaintEvent* /*event*/) {
    const model::Graphic* found = graphic();
    if (found == nullptr) {
        return;
    }
    const QRectF box = boxRect();
    if (box.isEmpty()) {
        return;
    }
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);

    // The guide first, under the box: it is about where the box has latched,
    // and the box is the thing being moved.
    painter.setPen(QPen{kGuide, 1.0, Qt::DashLine});
    if (snappedX_) {
        const double x = toWidget(*snappedX_, 0.0).x();
        painter.drawLine(QPointF{x, 0.0}, QPointF{x, static_cast<double>(height())});
    }
    if (snappedY_) {
        const double y = toWidget(0.0, *snappedY_).y();
        painter.drawLine(QPointF{0.0, y}, QPointF{static_cast<double>(width()), y});
    }

    // Drawn twice, dark under light: a hairline over a picture is invisible
    // against something the same brightness, and a title is usually over
    // something bright.
    painter.setPen(QPen{kShadow, 3.0});
    painter.drawRect(box);
    painter.setPen(QPen{kBox, 1.0});
    painter.drawRect(box);

    // No handles while it is being typed into: they are for a gesture that is
    // not available, and they sit exactly where the text does.
    if (isTyping()) {
        painter.setPen(QPen{kTyping, 1.0});
        painter.drawRect(box.adjusted(-2.0, -2.0, 2.0, 2.0));
        return;
    }

    painter.setPen(QPen{kShadow, 1.0});
    painter.setBrush(kHandle);
    for (const QPointF& corner :
         {box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight()}) {
        painter.drawRect(QRectF{corner.x() - 3.0, corner.y() - 3.0, 6.0, 6.0});
    }
}

}  // namespace zaro::app
