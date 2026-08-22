#include "ViewerOverlay.h"

#include <QFont>
#include <QFontDatabase>
#include <QPainter>
#include <QRectF>

#include "ProgramMonitor.h"
#include "Theme.h"

namespace zaro::app {

ViewerOverlay::ViewerOverlay(ProgramMonitor* monitor, QWidget* parent)
    : QWidget{parent}, monitor_{monitor} {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
}

void ViewerOverlay::setInfo(const QString& clipName, const QString& timecode, const QString& format,
                            const QString& level) {
    if (clipName_ == clipName && timecode_ == timecode && format_ == format && level_ == level) {
        return;
    }
    clipName_ = clipName;
    timecode_ = timecode;
    format_ = format;
    level_ = level;
    update();
}

void ViewerOverlay::setGuides(bool on) {
    if (guides_ == on) {
        return;
    }
    guides_ = on;
    update();
}

void ViewerOverlay::paintEvent(QPaintEvent* /*event*/) {
    if (monitor_ == nullptr) {
        return;
    }
    const QRectF picture = monitor_->pictureRect();
    if (picture.width() < 40.0 || picture.height() < 30.0) {
        return;
    }

    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (guides_) {
        // 5% action-safe, 10% title-safe, and the thirds: the three lines
        // anybody framing a shot is actually looking for.
        painter.setPen(QPen(theme::mix(theme::bg(), theme::accent(), 0.45), 1.0));
        painter.drawRect(picture.adjusted(picture.width() * 0.05, picture.height() * 0.05,
                                          -picture.width() * 0.05, -picture.height() * 0.05));
        painter.setPen(QPen(theme::mix(theme::bg(), theme::accent(), 0.28), 1.0));
        painter.drawRect(picture.adjusted(picture.width() * 0.10, picture.height() * 0.10,
                                          -picture.width() * 0.10, -picture.height() * 0.10));
        painter.setPen(QPen(theme::textAt(0.22), 1.0));
        for (int i = 1; i < 3; ++i) {
            const double x = picture.left() + picture.width() * i / 3.0;
            const double y = picture.top() + picture.height() * i / 3.0;
            painter.drawLine(QPointF(x, picture.top()), QPointF(x, picture.bottom()));
            painter.drawLine(QPointF(picture.left(), y), QPointF(picture.right(), y));
        }
    }

    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSizeF(9.5);
    painter.setFont(font);

    const QRectF inset = picture.adjusted(10, 8, -10, -8);
    // Drawn twice: the burn-in sits on the picture, and a picture can be any
    // colour. A shadow under it is what keeps white text readable on a white
    // frame without a box that hides part of the shot.
    const auto burn = [&](const QRectF& box, int flags, const QString& fragment, double opacity) {
        if (fragment.isEmpty()) {
            return;
        }
        painter.setPen(QColor(0, 0, 0, 170));
        painter.drawText(box.translated(0, 1), flags, fragment);
        QColor ink = theme::text();
        ink.setAlphaF(static_cast<float>(opacity));
        painter.setPen(ink);
        painter.drawText(box, flags, fragment);
    };

    burn(inset, Qt::AlignTop | Qt::AlignLeft, clipName_, 0.8);
    burn(inset, Qt::AlignTop | Qt::AlignRight, timecode_, 0.8);
    burn(inset, Qt::AlignBottom | Qt::AlignLeft, format_, 0.6);
    burn(inset, Qt::AlignBottom | Qt::AlignRight, level_, 0.6);
}

}  // namespace zaro::app
