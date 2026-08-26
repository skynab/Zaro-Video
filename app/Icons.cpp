#include "Icons.h"

#include <QColor>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>

#include "Theme.h"

namespace zaro::app::icons {
namespace {

/// Every path is drawn in this box and scaled to whatever size is asked for, so
/// one set of coordinates serves a 16px toolbar and a 32px one.
constexpr double kBox = 16.0;

/// An arrowhead at `tip`, its barbs opening back towards where the shaft came
/// from -- so the head points away from `towardsX`.
void arrowHead(QPainterPath& path, double tipX, double tipY, double towardsX, double size) {
    const double sign = towardsX > tipX ? 1.0 : -1.0;
    path.moveTo(tipX + sign * size, tipY - size);
    path.lineTo(tipX, tipY);
    path.lineTo(tipX + sign * size, tipY + size);
}

QPainterPath pathFor(Glyph glyph) {
    QPainterPath path;
    switch (glyph) {
        case Glyph::Cursor:
            // The pointer, drawn as the outline Phosphor's cursor is: a closed
            // arrow with the tail cut square.
            path.moveTo(3.4, 2.2);
            path.lineTo(12.9, 8.3);
            path.lineTo(8.5, 9.3);
            path.lineTo(10.9, 13.6);
            path.lineTo(8.8, 14.6);
            path.lineTo(6.5, 10.4);
            path.lineTo(3.4, 13.4);
            path.closeSubpath();
            return path;

        case Glyph::Scissors:
            // Two blades crossing above two finger holes.
            path.addEllipse(QPointF(4.4, 12.2), 2.0, 2.0);
            path.addEllipse(QPointF(11.6, 12.2), 2.0, 2.0);
            path.moveTo(5.8, 10.8);
            path.lineTo(12.4, 2.4);
            path.moveTo(10.2, 10.8);
            path.lineTo(3.6, 2.4);
            return path;

        case Glyph::TrimEdges:
            // A span with a hard edge at each end: the two cuts a trim moves
            // between.
            path.moveTo(2.2, 3.6);
            path.lineTo(2.2, 12.4);
            path.moveTo(13.8, 3.6);
            path.lineTo(13.8, 12.4);
            path.moveTo(5.0, 8.0);
            path.lineTo(11.0, 8.0);
            arrowHead(path, 4.6, 8.0, 11.0, 2.0);
            arrowHead(path, 11.4, 8.0, 4.6, 2.0);
            return path;

        case Glyph::SlipArrows:
            // The same movement without the end stops: what slips is the
            // content, not the edges.
            path.moveTo(3.4, 8.0);
            path.lineTo(12.6, 8.0);
            arrowHead(path, 2.6, 8.0, 12.6, 2.4);
            arrowHead(path, 13.4, 8.0, 2.6, 2.4);
            return path;

        case Glyph::Hand: {
            // Palm and thumb as one outline, three fingers on top of it.
            path.moveTo(4.3, 9.6);
            path.lineTo(4.3, 7.2);
            path.cubicTo(3.2, 7.6, 2.8, 8.6, 3.3, 9.6);
            path.lineTo(4.6, 12.0);
            path.cubicTo(5.4, 13.5, 6.9, 14.4, 8.6, 14.4);
            path.cubicTo(11.1, 14.4, 12.5, 12.9, 12.5, 10.5);
            path.lineTo(12.5, 6.6);
            path.moveTo(6.4, 8.0);
            path.lineTo(6.4, 3.4);
            path.moveTo(9.4, 8.0);
            path.lineTo(9.4, 2.6);
            path.moveTo(12.5, 8.0);
            path.lineTo(12.5, 4.0);
            return path;
        }

        case Glyph::Magnifier:
            path.addEllipse(QPointF(7.0, 7.0), 4.3, 4.3);
            path.moveTo(10.2, 10.2);
            path.lineTo(14.0, 14.0);
            return path;

        case Glyph::Magnet:
            // A horseshoe, poles down. The gap between the poles is the whole
            // idea of the glyph, so the inner arch is drawn rather than
            // implied by a thick stroke.
            path.moveTo(2.6, 13.2);
            path.lineTo(2.6, 8.0);
            path.cubicTo(2.6, 2.6, 13.4, 2.6, 13.4, 8.0);
            path.lineTo(13.4, 13.2);
            path.moveTo(6.1, 13.2);
            path.lineTo(6.1, 8.0);
            path.cubicTo(6.1, 5.6, 9.9, 5.6, 9.9, 8.0);
            path.lineTo(9.9, 13.2);
            path.moveTo(2.6, 13.2);
            path.lineTo(6.1, 13.2);
            path.moveTo(9.9, 13.2);
            path.lineTo(13.4, 13.2);
            // The poles, banded across each leg. Without them the glyph is an
            // arch, and an arch is a bridge.
            path.moveTo(2.6, 10.6);
            path.lineTo(6.1, 10.6);
            path.moveTo(9.9, 10.6);
            path.lineTo(13.4, 10.6);
            return path;

        case Glyph::Bookmark:
            path.moveTo(4.0, 2.6);
            path.lineTo(12.0, 2.6);
            path.lineTo(12.0, 13.8);
            path.lineTo(8.0, 10.5);
            path.lineTo(4.0, 13.8);
            path.closeSubpath();
            return path;

        case Glyph::Split:
            // Two pieces pulled apart, with the cut between them. Deliberately
            // not the scissors: the scissors is the blade *tool*, and two
            // buttons a centimetre apart wearing the same glyph is two
            // buttons nobody can tell apart.
            path.addRoundedRect(QRectF(2.2, 4.4, 4.4, 7.2), 1.2, 1.2);
            path.addRoundedRect(QRectF(9.4, 4.4, 4.4, 7.2), 1.2, 1.2);
            path.moveTo(8.0, 2.4);
            path.lineTo(8.0, 13.6);
            return path;

        case Glyph::CrossFade:
            // One level falling as the other rises: the shape of the thing
            // itself, and the symbol every mixer already uses for it.
            path.moveTo(2.4, 12.6);
            path.cubicTo(6.4, 12.6, 6.4, 3.4, 13.6, 3.4);
            path.moveTo(2.4, 3.4);
            path.cubicTo(6.4, 3.4, 6.4, 12.6, 13.6, 12.6);
            return path;

        case Glyph::Link:
            // Two capsules overlapping: a chain, at the smallest size that
            // still reads as one. This is drawn at nine pixels on a clip, so
            // anything with more detail in it becomes a smudge.
            path.addRoundedRect(QRectF(1.4, 5.2, 8.0, 5.6), 2.8, 2.8);
            path.addRoundedRect(QRectF(6.6, 5.2, 8.0, 5.6), 2.8, 2.8);
            return path;

        case Glyph::Plus:
            path.moveTo(8.0, 3.2);
            path.lineTo(8.0, 12.8);
            path.moveTo(3.2, 8.0);
            path.lineTo(12.8, 8.0);
            return path;

        case Glyph::Minus:
            path.moveTo(3.2, 8.0);
            path.lineTo(12.8, 8.0);
            return path;

        case Glyph::Heart:
            path.moveTo(8.0, 13.8);
            path.cubicTo(8.0, 13.8, 2.1, 10.3, 2.1, 6.3);
            path.cubicTo(2.1, 4.3, 3.7, 2.8, 5.6, 2.8);
            path.cubicTo(6.9, 2.8, 7.6, 3.7, 8.0, 4.4);
            path.cubicTo(8.4, 3.7, 9.1, 2.8, 10.4, 2.8);
            path.cubicTo(12.3, 2.8, 13.9, 4.3, 13.9, 6.3);
            path.cubicTo(13.9, 10.3, 8.0, 13.8, 8.0, 13.8);
            path.closeSubpath();
            return path;

        case Glyph::FilmStrip:
            // The frame, with the two divider lines and the gaps that make a
            // strip read as a strip at fifteen pixels. Phosphor perforates the
            // margins; at this size the perforations close up into a grey bar,
            // so the margins are broken instead.
            path.addRoundedRect(QRectF(2.0, 3.2, 12.0, 9.6), 1.6, 1.6);
            path.moveTo(5.6, 3.2);
            path.lineTo(5.6, 12.8);
            path.moveTo(10.4, 3.2);
            path.lineTo(10.4, 12.8);
            path.moveTo(2.0, 8.0);
            path.lineTo(5.6, 8.0);
            path.moveTo(10.4, 8.0);
            path.lineTo(14.0, 8.0);
            return path;

        case Glyph::Waveform: {
            // Bars either side of a centre line, tall in the middle. A drawn
            // envelope would be truer to the glyph and unreadable this small.
            constexpr double kHalf[] = {1.4, 3.2, 5.2, 3.8, 2.2, 4.6, 1.8};
            double x = 2.6;
            for (const double half : kHalf) {
                path.moveTo(x, 8.0 - half);
                path.lineTo(x, 8.0 + half);
                x += 1.8;
            }
            return path;
        }

        case Glyph::Image:
            path.addRoundedRect(QRectF(2.2, 3.2, 11.6, 9.6), 1.6, 1.6);
            path.addEllipse(QPointF(5.8, 6.4), 1.1, 1.1);
            path.moveTo(2.2, 11.0);
            path.lineTo(6.2, 7.6);
            path.lineTo(9.4, 10.4);
            path.lineTo(11.0, 9.0);
            path.lineTo(13.8, 11.4);
            return path;

        case Glyph::CaretDown:
            path.moveTo(4.2, 6.3);
            path.lineTo(8.0, 10.1);
            path.lineTo(11.8, 6.3);
            return path;

        case Glyph::CaretRight:
            path.moveTo(6.3, 4.2);
            path.lineTo(10.1, 8.0);
            path.lineTo(6.3, 11.8);
            return path;

        case Glyph::DotsThree:
            path.addEllipse(QPointF(3.6, 8.0), 1.15, 1.15);
            path.addEllipse(QPointF(8.0, 8.0), 1.15, 1.15);
            path.addEllipse(QPointF(12.4, 8.0), 1.15, 1.15);
            return path;

        case Glyph::Rows:
            path.moveTo(2.6, 4.4);
            path.lineTo(13.4, 4.4);
            path.moveTo(2.6, 8.0);
            path.lineTo(13.4, 8.0);
            path.moveTo(2.6, 11.6);
            path.lineTo(13.4, 11.6);
            return path;

        case Glyph::Circle:
            path.addEllipse(QPointF(8.0, 8.0), 5.2, 5.2);
            return path;

        case Glyph::CheckCircle:
            // The same ring with a tick in it. The design draws the lit state
            // at Phosphor's fill weight; stroked keeps it in one set with every
            // other icon here, and the chip behind it already lights up.
            path.addEllipse(QPointF(8.0, 8.0), 5.2, 5.2);
            path.moveTo(5.4, 8.1);
            path.lineTo(7.2, 9.9);
            path.lineTo(10.6, 6.3);
            return path;
    }
    return path;
}

/// The one glyph the design draws filled. Phosphor calls this weight `ph-fill`,
/// and a heart is the shape that reads as an outline the least.
bool isFilled(Glyph glyph) {
    return glyph == Glyph::Heart || glyph == Glyph::DotsThree;
}

}  // namespace

QPixmap pixmap(Glyph glyph, int size, const QColor& ink) {
    // Rendered at the ratio of the screen it will be shown on. A pixmap built
    // at one device pixel per logical pixel is a soft icon on every display
    // sold in the last decade.
    const qreal ratio = QGuiApplication::primaryScreen() != nullptr
                            ? QGuiApplication::primaryScreen()->devicePixelRatio()
                            : 1.0;
    QPixmap canvas{QSize{static_cast<int>(size * ratio), static_cast<int>(size * ratio)}};
    canvas.setDevicePixelRatio(ratio);
    canvas.fill(Qt::transparent);

    QPainter painter{&canvas};
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(size / kBox, size / kBox);

    // Stroked, not filled: Phosphor's regular weight is an outline, and a
    // filled cursor beside an outlined magnifier reads as two icon sets.
    if (isFilled(glyph)) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
    } else {
        QPen pen{ink};
        pen.setWidthF(1.5);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
    }
    painter.drawPath(pathFor(glyph));
    return canvas;
}

QIcon toolIcon(Glyph glyph, int size) {
    QIcon icon;
    icon.addPixmap(pixmap(glyph, size, theme::textAt(0.62)), QIcon::Normal, QIcon::Off);
    icon.addPixmap(pixmap(glyph, size, theme::text()), QIcon::Active, QIcon::Off);
    // On is the picked tool. The stylesheet tints the button behind it; the
    // icon has to move with it or a checked tool looks like a lit background
    // with somebody else's icon on top.
    icon.addPixmap(pixmap(glyph, size, theme::accent(200)), QIcon::Normal, QIcon::On);
    icon.addPixmap(pixmap(glyph, size, theme::accent(200)), QIcon::Active, QIcon::On);
    icon.addPixmap(pixmap(glyph, size, theme::textAt(0.30)), QIcon::Disabled, QIcon::Off);
    return icon;
}

}  // namespace zaro::app::icons
