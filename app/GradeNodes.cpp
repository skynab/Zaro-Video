#include "GradeNodes.h"

#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>

#include "Theme.h"

namespace zaro::app {
namespace {

constexpr int kNodeWidth = 58;
constexpr int kNodeHeight = 50;
constexpr int kPanelHeight = 118;

icons::Glyph glyphFor(GradeNodes::Stage stage) {
    switch (stage) {
        case GradeNodes::Stage::Primary:
            return icons::Glyph::CircleHalf;
        case GradeNodes::Stage::Curves:
            return icons::Glyph::BezierCurve;
        case GradeNodes::Stage::Secondary:
            return icons::Glyph::Selection;
        case GradeNodes::Stage::Look:
            return icons::Glyph::Swap;
    }
    return icons::Glyph::CircleHalf;
}

}  // namespace

GradeNodes::GradeNodes(QWidget* parent) : QWidget{parent} {
    setObjectName("grade-nodes");
    setFixedHeight(kPanelHeight);
    setCursor(Qt::PointingHandCursor);
    occupied_.fill(false);
}

QSize GradeNodes::sizeHint() const {
    return QSize{276, kPanelHeight};
}

QString GradeNodes::nameOf(Stage stage) {
    switch (stage) {
        case Stage::Primary:
            return QStringLiteral("Primary");
        case Stage::Curves:
            return QStringLiteral("Curves");
        case Stage::Secondary:
            return QStringLiteral("Secondary");
        case Stage::Look:
            return QStringLiteral("Look");
    }
    return {};
}

void GradeNodes::setStage(Stage stage) {
    stage_ = stage;
    update();
}

void GradeNodes::setOccupied(std::array<bool, kStageCount> occupied) {
    occupied_ = occupied;
    update();
}

void GradeNodes::setEnabledChain(bool enabled) {
    enabled_ = enabled;
    update();
}

/// Four boxes spread across whatever width the panel got, with the wire running
/// through them. Laid out from the widget rather than from fixed offsets: the
/// design's panel is 276 wide and this one is whatever the splitter left.
QRect GradeNodes::nodeRect(int at) const {
    const int usable = width() - 24;
    const int step = kStageCount > 1 ? (usable - kNodeWidth) / (kStageCount - 1) : 0;
    return QRect{12 + (at * step), (kPanelHeight - kNodeHeight) / 2, kNodeWidth, kNodeHeight};
}

void GradeNodes::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);

    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::well());
    painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 7, 7);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen{theme::divider(), 1.0});
    painter.drawRoundedRect(QRectF{rect()}.adjusted(0.5, 0.5, -0.5, -0.5), 7, 7);

    const int middle = kPanelHeight / 2;
    const QColor wire = enabled_ ? theme::mix(theme::well(), theme::text(), 0.30)
                                 : theme::mix(theme::well(), theme::text(), 0.14);

    // The wire first, so the boxes sit on it rather than beside it.
    painter.setPen(QPen{wire, 1.5});
    painter.drawLine(4, middle, nodeRect(0).left(), middle);
    for (int at = 0; at + 1 < kStageCount; ++at) {
        painter.drawLine(nodeRect(at).right(), middle, nodeRect(at + 1).left(), middle);
    }
    painter.drawLine(nodeRect(kStageCount - 1).right(), middle, width() - 4, middle);
    painter.setPen(Qt::NoPen);
    painter.setBrush(wire);
    painter.drawEllipse(QPointF{4.0, static_cast<double>(middle)}, 3.5, 3.5);
    painter.drawEllipse(QPointF{width() - 4.0, static_cast<double>(middle)}, 3.5, 3.5);

    QFont label = font();
    label.setPointSizeF(7.0);

    for (int at = 0; at < kStageCount; ++at) {
        const auto stage = static_cast<Stage>(at);
        const QRect box = nodeRect(at);
        QLinearGradient face{box.topLeft(), box.bottomLeft()};
        face.setColorAt(0.0, theme::neutral(800));
        face.setColorAt(1.0, theme::neutral(900));
        painter.setPen(Qt::NoPen);
        painter.setBrush(face);
        painter.drawRoundedRect(box, 5, 5);

        painter.setBrush(Qt::NoBrush);
        painter.setPen(stage == stage_ && enabled_
                           ? QPen{theme::accent(300), 1.5}
                           : QPen{theme::mix(theme::neutral(900), theme::text(), 0.12), 1.0});
        painter.drawRoundedRect(QRectF{box}.adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);

        // Lit when the stage has something in it, muted when it is a pass
        // through. That is the whole reading of the picture: which of these
        // four has this shot been through.
        const QColor ink = !enabled_                                 ? theme::textAt(0.22)
                           : occupied_[static_cast<std::size_t>(at)] ? theme::accent(200)
                                                                     : theme::textAt(0.40);
        painter.drawPixmap(QPoint{box.center().x() - 7, box.top() + 9},
                           icons::pixmap(glyphFor(stage), 14, ink));

        painter.setFont(label);
        painter.setPen(enabled_ ? theme::textAt(0.60) : theme::textAt(0.25));
        painter.drawText(QRect{box.left(), box.bottom() - 17, box.width(), 14},
                         Qt::AlignHCenter | Qt::AlignVCenter, nameOf(stage));
    }
}

void GradeNodes::mousePressEvent(QMouseEvent* event) {
    if (!enabled_) {
        return;
    }
    for (int at = 0; at < kStageCount; ++at) {
        if (nodeRect(at).contains(event->pos())) {
            stage_ = static_cast<Stage>(at);
            update();
            emit stageChosen(at);
            return;
        }
    }
}

}  // namespace zaro::app
