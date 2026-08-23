#include "SupportButton.h"

#include <QFontMetrics>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

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
}

QSize SupportButton::sizeHint() const {
    const int textWidth = fontMetrics().horizontalAdvance(text());
    return {kPadding * 2 + kIconSize + kGap + textWidth, 30};
}

void SupportButton::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);

    QLinearGradient ring{QPointF(0, 0), QPointF(width(), 0)};
    const auto stops = static_cast<int>(std::size(kSpectrum));
    for (int i = 0; i < stops; ++i) {
        QColor colour = kSpectrum[i];
        // Hover brightens the ring and a press dims it, which is the whole of
        // this button's state: it has nothing to be checked or disabled about.
        if (underMouse() && !isDown()) {
            colour = colour.lighter(112);
        } else if (isDown()) {
            colour = colour.darker(112);
        }
        ring.setColorAt(static_cast<double>(i) / (stops - 1), colour);
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
