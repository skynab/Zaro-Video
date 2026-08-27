#include "FrameThumb.h"

#include <QPainter>
#include <QPainterPath>
#include <algorithm>

#include "Theme.h"

namespace zaro::app {
namespace {

// Wide enough that a 16:9 frame is legible at the width the Audio column gets,
// and no wider: this is a glance, not a picture to judge anything on.
constexpr int kThumbWidth = 238;
constexpr int kMargin = 12;
constexpr int kRadius = 6;

}  // namespace

FrameThumb::FrameThumb(QWidget* parent) : QWidget{parent} {
    setObjectName("frame-thumb");
    setAttribute(Qt::WA_StyledBackground, true);
    setToolTip("What the playhead is on");
}

QSize FrameThumb::sizeHint() const {
    return QSize{kThumbWidth + (kMargin * 2), heightForWidth(kThumbWidth + (kMargin * 2))};
}

int FrameThumb::heightForWidth(int width) const {
    const int picture = std::max(1, width - (kMargin * 2));
    return ((picture * 9) / 16) + (kMargin * 2);
}

QRect FrameThumb::pictureRect() const {
    const int wide = std::max(1, width() - (kMargin * 2));
    return QRect{kMargin, kMargin, wide, (wide * 9) / 16};
}

void FrameThumb::setFrame(const QImage& frame) {
    if (frame.isNull()) {
        clearFrame();
        return;
    }
    // Held at the size it is shown, not at the size it arrived: a full frame per
    // workspace switch is megabytes kept for a thumbnail.
    frame_ = frame.scaled(QSize{kThumbWidth * 2, kThumbWidth * 2}, Qt::KeepAspectRatio,
                          Qt::SmoothTransformation);
    update();
}

void FrameThumb::clearFrame() {
    frame_ = QImage{};
    update();
}

void FrameThumb::setCaption(const QString& name, const QString& timecode) {
    name_ = name;
    timecode_ = timecode;
    update();
}

void FrameThumb::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRect picture = pictureRect();
    QPainterPath frame;
    frame.addRoundedRect(picture, kRadius, kRadius);
    painter.setClipPath(frame);

    if (frame_.isNull()) {
        painter.fillRect(picture, theme::well());
        painter.setClipping(false);
        QFont note = font();
        note.setPointSizeF(8.0);
        painter.setFont(note);
        painter.setPen(theme::textAt(0.32));
        painter.drawText(picture, Qt::AlignCenter, "No picture at the playhead");
    } else {
        painter.fillRect(picture, theme::well());
        // Fitted rather than filled: a letterboxed thumbnail is honest about the
        // frame's shape, and a cropped one quietly hides what is at the edges.
        const QSize fitted = frame_.size().scaled(picture.size(), Qt::KeepAspectRatio);
        const QRect into{picture.left() + ((picture.width() - fitted.width()) / 2),
                         picture.top() + ((picture.height() - fitted.height()) / 2),
                         fitted.width(), fitted.height()};
        painter.drawImage(into, frame_);

        // The caption sits on the picture, as the design draws it, over a wash
        // dark enough that it reads on a white frame as well as a black one.
        QLinearGradient shade{QPointF{0, static_cast<double>(picture.bottom() - 26)},
                              QPointF{0, static_cast<double>(picture.bottom())}};
        shade.setColorAt(0.0, QColor{0, 0, 0, 0});
        shade.setColorAt(1.0, QColor{0, 0, 0, 150});
        painter.setPen(Qt::NoPen);
        painter.setBrush(shade);
        painter.drawRect(QRect{picture.left(), picture.bottom() - 26, picture.width(), 27});
        painter.setClipping(false);

        QFont mono{QStringLiteral("Menlo")};
        mono.setStyleHint(QFont::Monospace);
        mono.setPointSizeF(7.5);
        painter.setFont(mono);
        const QFontMetrics metrics{mono};
        const QRect caption{picture.left() + 8, picture.bottom() - 20, picture.width() - 16, 16};
        painter.setPen(theme::textAt(0.72));
        painter.drawText(caption, Qt::AlignRight | Qt::AlignVCenter, timecode_);
        const int room = caption.width() - metrics.horizontalAdvance(timecode_) - 10;
        painter.drawText(caption, Qt::AlignLeft | Qt::AlignVCenter,
                         metrics.elidedText(name_, Qt::ElideMiddle, std::max(0, room)));
    }

    painter.setClipping(false);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen{theme::divider(), 1.0});
    painter.drawRoundedRect(QRectF{picture}.adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);
}

}  // namespace zaro::app
