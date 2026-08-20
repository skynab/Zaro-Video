#include "zaro/platform/qtext/QtTextRasterizer.h"

#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QRectF>
#include <QString>

namespace zaro::platform::qtext {

Status QtTextRasterizer::renderCoverage(const model::Graphic& graphic,
                                        render::RgbaImage& coverage) {
    if (!coverage.isValid()) {
        return Error{ErrorCode::InvalidData, "the coverage buffer has no size"};
    }
    if (QGuiApplication::instance() == nullptr) {
        // Qt's font engine needs an application object. Saying so is better
        // than the crash that follows, and it is the difference between "this
        // tool needs one line of setup" and "this tool is broken".
        return Error{ErrorCode::Unsupported,
                     "text rendering needs a QGuiApplication; construct one first"};
    }

    QImage mask(coverage.width(), coverage.height(), QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    {
        QPainter painter(&mask);
        // Antialiasing on, subpixel positioning off. Subpixel antialiasing is
        // specific to one screen's pixel layout and is wrong the moment the
        // result is composited or delivered anywhere else.
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);

        QFont font;
        if (!graphic.family.empty()) {
            font.setFamily(QString::fromStdString(graphic.family));
        }
        // Pixels, not points: a point size depends on a notional DPI, and a
        // title has to be the same size in a delivered frame whatever the
        // machine that rendered it thought its screen was.
        font.setPixelSize(std::max(1, static_cast<int>(graphic.pointSize)));
        font.setBold(graphic.bold);
        font.setItalic(graphic.italic);
        painter.setFont(font);

        // White: the glyphs are a coverage mask, and the colour comes from the
        // graphic in linear light afterwards.
        painter.setPen(Qt::white);

        // The graphic's box, in the same centre-origin coordinates the shapes
        // and the transform use.
        const double left = (coverage.width() * 0.5) + graphic.centreX - (graphic.width * 0.5);
        const double top = (coverage.height() * 0.5) + graphic.centreY - (graphic.height * 0.5);
        const QRectF box(left, top, graphic.width, graphic.height);

        const int horizontal = graphic.alignment < 0   ? Qt::AlignLeft
                               : graphic.alignment > 0 ? Qt::AlignRight
                                                       : Qt::AlignHCenter;
        painter.drawText(box, horizontal | Qt::AlignVCenter | Qt::TextWordWrap,
                         QString::fromStdString(graphic.text));
    }

    // Alpha only. The colour channels hold premultiplied white, which is the
    // same number -- but reading alpha says what is meant.
    for (std::int32_t y = 0; y < coverage.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(mask.constScanLine(y));
        render::Rgba* row = coverage.row(y);
        for (std::int32_t x = 0; x < coverage.width(); ++x) {
            row[x] = render::Rgba{0.0F, 0.0F, 0.0F, static_cast<float>(qAlpha(line[x])) / 255.0F};
        }
    }
    return {};
}

}  // namespace zaro::platform::qtext
