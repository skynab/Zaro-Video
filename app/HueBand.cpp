#include "HueBand.h"

#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace zaro::app {
namespace {

/// The shorter way round the circle, matching render::qualifierMask. Drawn
/// selection and actual selection have to be the same shape or the band is
/// worse than no band.
double hueDistance(double from, double to) {
    double difference = std::fabs(from - to);
    if (difference > 180.0) {
        difference = 360.0 - difference;
    }
    return difference;
}

}  // namespace

HueBand::HueBand(QWidget* parent) : QWidget{parent} {
    setMinimumHeight(22);
}

QSize HueBand::sizeHint() const {
    return {180, 24};
}

void HueBand::setWindow(double centre, double width, double softness) {
    centre_ = centre;
    width_ = width;
    softness_ = softness;
    update();
}

double HueBand::hueAt(int x) const {
    if (width() <= 1) {
        return 0.0;
    }
    return std::clamp(static_cast<double>(x) / (width() - 1), 0.0, 1.0) * 360.0;
}

void HueBand::mousePressEvent(QMouseEvent* event) {
    emit centreChanged(hueAt(static_cast<int>(event->position().x())));
}

void HueBand::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons().testFlag(Qt::LeftButton)) {
        emit centreChanged(hueAt(static_cast<int>(event->position().x())));
    }
}

void HueBand::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    const QRect area = rect();

    for (int x = 0; x < area.width(); ++x) {
        const double hue = hueAt(x);
        const double distance = hueDistance(hue, centre_);
        const double inner = std::clamp(width_, 0.0, 360.0) / 2.0;
        const double outer = inner + std::max(0.0, softness_);

        // Outside the window the band is dimmed rather than hidden: what is
        // *not* selected is as much a part of reading a qualifier as what is.
        double strength = 1.0;
        if (inner < 180.0) {
            if (distance >= outer) {
                strength = 0.0;
            } else if (distance > inner && outer > inner) {
                const double t = (outer - distance) / (outer - inner);
                strength = t * t * (3.0 - (2.0 * t));
            }
        }

        QColor colour = QColor::fromHsvF(static_cast<float>(hue / 360.0), 0.85F, 0.95F);
        const auto dim = static_cast<int>(60 + (strength * 195));
        colour = colour.darker(static_cast<int>(100 + ((1.0 - strength) * 160)));
        colour.setAlpha(dim);
        painter.setPen(colour);
        painter.drawLine(area.left() + x, area.top(), area.left() + x, area.bottom());
    }

    // The centre, so a narrow window is still visible when the band around it
    // is only a few pixels wide.
    const int centreX =
        static_cast<int>((std::fmod(centre_ + 360.0, 360.0) / 360.0) * (area.width() - 1));
    painter.setPen(QPen(QColor(250, 250, 255), 1));
    painter.drawLine(centreX, area.top(), centreX, area.bottom());
}

}  // namespace zaro::app
