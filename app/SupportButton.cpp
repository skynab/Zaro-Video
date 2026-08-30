#include "SupportButton.h"

#include <QFontMetrics>
#include <QHideEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QShowEvent>
#include <QTimer>
#include <cmath>

#include "Icons.h"
#include "Theme.h"

namespace zaro::app {
namespace {

/// The ring, as the design writes it: a full spectrum turned down to something
/// that can sit next to a monochrome tool bar without shouting.
const QColor kSpectrum[] = {
    QColor{0xd9, 0x6a, 0x6a}, QColor{0xd9, 0xa8, 0x6a}, QColor{0xd9, 0xc7, 0x6a},
    QColor{0x7f, 0xd9, 0x6a}, QColor{0x6a, 0xd9, 0xc7}, QColor{0x6a, 0x8f, 0xd9},
    QColor{0xa8, 0x6a, 0xd9},
};

const QColor kHeart{0xd9, 0x6a, 0x9c};

/// One lap of the spectrum, and how often it is redrawn. Slow enough to read
/// as drifting rather than as flashing -- the point is to catch an eye that
/// happens to pass, not to hold one.
constexpr int kCycleMs = 9000;
constexpr int kFrameMs = 33;

/// How many stops the travelling ring is drawn with. The spectrum is sampled
/// rather than mapped one-colour-to-one-stop, because a gradient that moves has
/// to wrap: the colour leaving the right edge is the one arriving at the left.
constexpr int kRingStops = 28;

/// The spectrum as a loop, sampled at `t` in [0,1) with the ends joined.
QColor spectrumAt(double t) {
    const auto count = static_cast<int>(std::size(kSpectrum));
    const double scaled = (t - std::floor(t)) * count;
    const auto first = static_cast<int>(scaled) % count;
    const int second = (first + 1) % count;
    // Mixed in float, which is what QColor's component accessors are: doing it
    // in double only to hand the result back as float is a conversion at every
    // stop and a warning at every build.
    const auto mix = static_cast<float>(scaled - std::floor(scaled));
    const QColor& a = kSpectrum[first];
    const QColor& b = kSpectrum[second];
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * mix,
                            a.greenF() + (b.greenF() - a.greenF()) * mix,
                            a.blueF() + (b.blueF() - a.blueF()) * mix);
}

constexpr double kRingWidth = 1.5;
constexpr double kRadius = 8.0;
constexpr int kIconSize = 14;
constexpr int kGap = 6;
constexpr int kPadding = 12;

}  // namespace

SupportButton::SupportButton(QWidget* parent) : QPushButton{parent} {
    setFlat(true);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);

    // Not started here: showEvent does that, so a button built into a workspace
    // nobody has opened yet costs nothing until it is looked at.
    clock_ = new QTimer(this);
    clock_->setInterval(kFrameMs);
    connect(clock_, &QTimer::timeout, this, [this] {
        phase_ += static_cast<double>(kFrameMs) / kCycleMs;
        phase_ -= std::floor(phase_);
        update();
    });
}

void SupportButton::showEvent(QShowEvent* event) {
    QPushButton::showEvent(event);
    clock_->start();
}

void SupportButton::hideEvent(QHideEvent* event) {
    QPushButton::hideEvent(event);
    clock_->stop();
}

QSize SupportButton::sizeHint() const {
    const int textWidth = fontMetrics().horizontalAdvance(text());
    return {kPadding * 2 + kIconSize + kGap + textWidth, 30};
}

void SupportButton::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);

    QLinearGradient ring{QPointF(0, 0), QPointF(width(), 0)};
    for (int i = 0; i <= kRingStops; ++i) {
        const double along = static_cast<double>(i) / kRingStops;
        // Minus the phase, so the colours travel left to right: the sample
        // taken from further back in the spectrum arrives here as time passes.
        QColor colour = spectrumAt(along - phase_);
        // Hover brightens the ring and a press dims it, which is the whole of
        // this button's state: it has nothing to be checked or disabled about.
        if (underMouse() && !isDown()) {
            colour = colour.lighter(112);
        } else if (isDown()) {
            colour = colour.darker(112);
        }
        ring.setColorAt(along, colour);
    }

    const QRectF outer = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ring);
    painter.drawRoundedRect(outer, kRadius, kRadius);

    const QRectF inner = outer.adjusted(kRingWidth, kRingWidth, -kRingWidth, -kRingWidth);
    painter.setBrush(theme::bg());
    painter.drawRoundedRect(inner, kRadius - kRingWidth, kRadius - kRingWidth);

    // Icon and label as one block, centred together: centring each in its own
    // half leaves a gap that grows with the label.
    const int textWidth = fontMetrics().horizontalAdvance(text());
    const double blockWidth = kIconSize + kGap + textWidth;
    const double left = inner.center().x() - blockWidth / 2.0;
    const QPixmap heart = icons::pixmap(icons::Glyph::Heart, kIconSize, kHeart);
    painter.drawPixmap(QPointF(left, inner.center().y() - kIconSize / 2.0), heart);
    painter.setPen(theme::text());
    painter.drawText(QRectF(left + kIconSize + kGap, inner.top(), textWidth, inner.height()),
                     Qt::AlignVCenter | Qt::AlignLeft, text());
}

}  // namespace zaro::app
