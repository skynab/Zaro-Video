#include "ColorWheel.h"

#include <QConicalGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QRadialGradient>
#include <algorithm>
#include <cmath>

#include "Theme.h"

namespace zaro::app {
namespace {

// The design's geometry, in logical pixels.
constexpr int kDisc = 104;
constexpr int kPuck = 13;
constexpr int kTrackHeight = 3;
constexpr int kNameGap = 7;
constexpr int kLabelHeight = 14;
constexpr int kReadoutHeight = 13;

/// The rim, as the design's conic gradient writes it: red at the top, round
/// through yellow, green, cyan, blue and magenta, back to red.
void addHueStops(QConicalGradient& wheel) {
    static const struct {
        double at;
        QColor colour;
    } kStops[] = {
        {0.0, QColor{0xd9, 0x6a, 0x6a}},   {1.0 / 6.0, QColor{0xd9, 0xb6, 0x6a}},
        {2.0 / 6.0, QColor{0x7f, 0xd9, 0x8f}}, {3.0 / 6.0, QColor{0x6a, 0xc7, 0xd9}},
        {4.0 / 6.0, QColor{0x7f, 0x8f, 0xd9}}, {5.0 / 6.0, QColor{0xd9, 0x6a, 0xc7}},
        {1.0, QColor{0xd9, 0x6a, 0x6a}},
    };
    for (const auto& stop : kStops) {
        wheel.setColorAt(stop.at, stop.colour);
    }
}

}  // namespace

ColorWheel::ColorWheel(QString name, QWidget* parent) : QWidget{parent}, name_{std::move(name)} {
    setCursor(Qt::CrossCursor);
    setFocusPolicy(Qt::NoFocus);
    setToolTip(name_ + " — drag the disc to balance, the bar below for master, double-click to reset");
}

QSize ColorWheel::sizeHint() const {
    return QSize{kDisc,
                 kDisc + kNameGap + kLabelHeight + kNameGap + kTrackHeight + kNameGap +
                     kReadoutHeight};
}

QRect ColorWheel::discRect() const {
    const int left = (width() - kDisc) / 2;
    return QRect{left, 0, kDisc, kDisc};
}

QRect ColorWheel::masterRect() const {
    const int left = (width() - kDisc) / 2;
    const int top = kDisc + kNameGap + kLabelHeight + kNameGap;
    return QRect{left, top, kDisc, kTrackHeight};
}

void ColorWheel::setBalance(double x, double y) {
    x_ = std::clamp(x, -1.0, 1.0);
    y_ = std::clamp(y, -1.0, 1.0);
    update();
}

void ColorWheel::setMaster(double master) {
    master_ = std::clamp(master, -1.0, 1.0);
    update();
}

void ColorWheel::reset() {
    x_ = 0.0;
    y_ = 0.0;
    master_ = 0.0;
    update();
    emit changed(true);
}

void ColorWheel::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRect disc = discRect();
    const QPointF centre = QRectF{disc}.center();
    const double radius = kDisc / 2.0;

    QConicalGradient wheel{centre, 90.0};
    addHueStops(wheel);
    painter.setPen(Qt::NoPen);
    painter.setBrush(wheel);
    painter.drawEllipse(disc);

    // The design darkens the disc from the edge inwards, so the saturated
    // colour survives only at the rim. Without it the puck sits on a field of
    // colour and the neutral centre -- the thing the whole control is measured
    // against -- is the hardest place on it to see.
    QRadialGradient falloff{centre, radius};
    QColor ground = theme::bg();
    falloff.setColorAt(0.0, ground);
    falloff.setColorAt(0.55, ground);
    ground.setAlpha(0);
    falloff.setColorAt(1.0, ground);
    painter.setBrush(falloff);
    painter.drawEllipse(disc);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen{theme::mix(theme::surface(), theme::text(), 0.12), 1.0});
    painter.drawEllipse(QRectF{disc}.adjusted(0.5, 0.5, -0.5, -0.5));

    // The puck. Y is negated because the balance is read the way a graph is and
    // drawn the way a screen is.
    const QPointF at{centre.x() + (x_ * (radius - kPuck / 2.0)),
                     centre.y() - (y_ * (radius - kPuck / 2.0))};
    painter.setPen(QPen{theme::bg(), 2.0});
    painter.setBrush(theme::text());
    painter.drawEllipse(at, kPuck / 2.0, kPuck / 2.0);

    QFont label = font();
    label.setPointSizeF(8.5);
    painter.setFont(label);
    painter.setPen(theme::textAt(0.68));
    painter.drawText(QRect{0, kDisc + kNameGap, width(), kLabelHeight},
                     Qt::AlignHCenter | Qt::AlignVCenter, name_);

    const QRect track = masterRect();
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::neutral(800));
    painter.drawRoundedRect(track, 2, 2);
    const double masterAt = track.left() + ((master_ + 1.0) / 2.0) * track.width();
    painter.setBrush(theme::accent(300));
    painter.drawEllipse(QPointF{masterAt, track.center().y() + 0.5}, 4.5, 4.5);

    QFont readout{QStringLiteral("Menlo")};
    readout.setStyleHint(QFont::Monospace);
    readout.setPointSizeF(7.5);
    painter.setFont(readout);
    painter.setPen(theme::textAt(0.45));
    painter.drawText(
        QRect{0, track.bottom() + kNameGap, width(), kReadoutHeight},
        Qt::AlignHCenter | Qt::AlignVCenter,
        QString("%1  %2  %3")
            .arg(x_, 0, 'f', 2)
            .arg(y_, 0, 'f', 2)
            .arg(master_, 0, 'f', 2));
}

/// Where in the disc a point is, as a balance. Clamped to the rim rather than
/// ignored past it: a drag that leaves the circle should pin to the edge, which
/// is what every other wheel does and what stops a grade jumping back to centre
/// because somebody's hand moved two pixels too far.
void ColorWheel::takeDisc(const QPoint& where) {
    const QRect disc = discRect();
    const QPointF centre = QRectF{disc}.center();
    const double radius = (kDisc / 2.0) - (kPuck / 2.0);
    double dx = (where.x() - centre.x()) / radius;
    double dy = (centre.y() - where.y()) / radius;
    if (const double length = std::hypot(dx, dy); length > 1.0) {
        dx /= length;
        dy /= length;
    }
    x_ = dx;
    y_ = dy;
    update();
}

void ColorWheel::takeMaster(const QPoint& where) {
    const QRect track = masterRect();
    const double fraction =
        std::clamp(static_cast<double>(where.x() - track.left()) / track.width(), 0.0, 1.0);
    master_ = (fraction * 2.0) - 1.0;
    update();
}

void ColorWheel::mousePressEvent(QMouseEvent* event) {
    // The master's hit area is taller than the three pixels it is drawn as: a
    // three-pixel target is one nobody can hit.
    if (masterRect().adjusted(-2, -8, 2, 8).contains(event->pos())) {
        grab_ = Grab::Master;
        takeMaster(event->pos());
    } else if (discRect().contains(event->pos())) {
        grab_ = Grab::Disc;
        takeDisc(event->pos());
    } else {
        return;
    }
    emit changed(false);
}

void ColorWheel::mouseMoveEvent(QMouseEvent* event) {
    if (grab_ == Grab::Disc) {
        takeDisc(event->pos());
    } else if (grab_ == Grab::Master) {
        takeMaster(event->pos());
    } else {
        return;
    }
    emit changed(false);
}

void ColorWheel::mouseReleaseEvent(QMouseEvent* /*event*/) {
    if (grab_ == Grab::None) {
        return;
    }
    grab_ = Grab::None;
    emit changed(true);
}

void ColorWheel::mouseDoubleClickEvent(QMouseEvent* event) {
    if (discRect().contains(event->pos()) || masterRect().adjusted(-2, -8, 2, 8).contains(event->pos())) {
        reset();
    }
}

}  // namespace zaro::app
