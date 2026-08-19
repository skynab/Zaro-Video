#include "ScopesPanel.h"

#include <QComboBox>
#include <QPainter>
#include <QShowEvent>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace zaro::app {
namespace {

const QColor kBackground{16, 16, 20};
const QColor kGraticule{70, 70, 82};
const QColor kTrace{210, 230, 210};
const QColor kRed{232, 96, 96};
const QColor kGreen{96, 216, 128};
const QColor kBlue{110, 150, 245};
const QColor kLabel{170, 170, 182};

/// A trace is drawn with the square root of its count.
///
/// A waveform's counts span orders of magnitude: a flat sky puts thousands of
/// pixels on one level while a highlight puts three on another. Linear scaling
/// makes everything but the densest line invisible, which is exactly the
/// detail an instrument exists to show.
float traceAlpha(std::uint32_t count, std::uint32_t peak) {
    if (count == 0 || peak == 0) {
        return 0.0F;
    }
    const float ratio = static_cast<float>(count) / static_cast<float>(peak);
    return std::clamp(std::sqrt(ratio), 0.05F, 1.0F);
}

}  // namespace

ScopesPanel::ScopesPanel(QWidget* parent) : QWidget{parent} {
    chooser_ = new QComboBox(this);
    chooser_->addItem("Waveform", static_cast<int>(Mode::Waveform));
    chooser_->addItem("RGB Parade", static_cast<int>(Mode::Parade));
    chooser_->addItem("Histogram", static_cast<int>(Mode::Histogram));
    chooser_->addItem("Vectorscope", static_cast<int>(Mode::Vectorscope));
    chooser_->setObjectName("scope-chooser");

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(chooser_);
    layout->addStretch(1);

    connect(chooser_, &QComboBox::currentIndexChanged, this, [this] {
        mode_ = static_cast<Mode>(chooser_->currentData().toInt());
        // Every instrument is computed in one pass, so switching does not need
        // a new measurement -- but the panel may have been hidden when the last
        // one was taken.
        if (!hasScopes_) {
            emit measurementNeeded();
        }
        update();
    });

    setMinimumHeight(160);
}

bool ScopesPanel::wantsMeasurement() const {
    return isVisible();
}

void ScopesPanel::setScopes(render::FrameScopes scopes) {
    scopes_ = std::move(scopes);
    hasScopes_ = true;
    update();
}

void ScopesPanel::clear() {
    hasScopes_ = false;
    update();
}

void ScopesPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // Nothing is measured while the panel is hidden, so it arrives empty.
    emit measurementNeeded();
}

QRect ScopesPanel::plotArea() const {
    return rect().adjusted(4, chooser_->height() + 8, -4, -4);
}

void ScopesPanel::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    const QRect area = plotArea();
    painter.fillRect(area, kBackground);

    if (!hasScopes_) {
        painter.setPen(kLabel);
        painter.drawText(area, Qt::AlignCenter, "No frame measured");
        return;
    }

    switch (mode_) {
        case Mode::Waveform:
            paintWaveform(painter, area, scopes_.luma, kTrace);
            break;
        case Mode::Parade: {
            // Side by side in R, G, B order, the way a parade is read.
            const int third = area.width() / 3;
            paintWaveform(painter, QRect(area.left(), area.top(), third, area.height()),
                          scopes_.red, kRed);
            paintWaveform(painter, QRect(area.left() + third, area.top(), third, area.height()),
                          scopes_.green, kGreen);
            paintWaveform(painter,
                          QRect(area.left() + (2 * third), area.top(), third, area.height()),
                          scopes_.blue, kBlue);
            break;
        }
        case Mode::Histogram:
            paintHistogram(painter, area);
            break;
        case Mode::Vectorscope:
            paintVectorscope(painter, area);
            break;
    }
}

void ScopesPanel::paintWaveform(QPainter& painter, const QRect& area,
                                const render::Waveform& waveform, const QColor& colour) const {
    if (!waveform.isValid() || area.width() <= 0 || area.height() <= 0) {
        return;
    }
    painter.save();
    painter.setClipRect(area);

    // Graticule at the levels that matter: black, the legal floor and ceiling,
    // and white.
    painter.setPen(kGraticule);
    for (const int level : {0, 16, 128, 235, 255}) {
        const int y = area.bottom() - ((level * (area.height() - 1)) / 255);
        painter.drawLine(area.left(), y, area.right(), y);
    }

    const std::uint32_t peak = waveform.peak();
    for (int x = 0; x < area.width(); ++x) {
        // The panel is not the measurement's width, so columns are mapped
        // rather than assumed to line up.
        const auto column = static_cast<std::int32_t>(static_cast<double>(x) * waveform.columns() /
                                                      std::max(1, area.width()));
        for (std::int32_t level = 0; level < render::Waveform::kLevels; ++level) {
            const float alpha = traceAlpha(waveform.at(column, level), peak);
            if (alpha <= 0.0F) {
                continue;
            }
            // Level 0 is black, and black belongs at the bottom: the
            // measurement is in signal order and the screen is upside down
            // relative to it.
            // height() - 1, not height(): the bottom and top rows are both
            // inside the area, so the span between them is one less than the
            // count of rows. Using the count puts white one pixel above the
            // plot, where it is clipped away -- a fully lit frame drew
            // nothing at all.
            const int y =
                area.bottom() - ((level * (area.height() - 1)) / (render::Waveform::kLevels - 1));
            QColor pen = colour;
            pen.setAlphaF(alpha);
            painter.setPen(pen);
            painter.drawPoint(area.left() + x, y);
        }
    }
    painter.restore();
}

void ScopesPanel::paintHistogram(QPainter& painter, const QRect& area) const {
    if (!scopes_.histogram.isValid() || scopes_.histogram.peak == 0) {
        return;
    }
    painter.save();
    painter.setClipRect(area);
    painter.setPen(kGraticule);
    for (int i = 1; i < 4; ++i) {
        const int x = area.left() + ((area.width() * i) / 4);
        painter.drawLine(x, area.top(), x, area.bottom());
    }

    const auto draw = [&](const std::vector<std::uint32_t>& bins, const QColor& colour) {
        QColor fill = colour;
        // Additive-looking overlap, so where the three channels agree reads as
        // neutral rather than as whichever was drawn last.
        fill.setAlphaF(0.55F);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        QPolygonF shape;
        shape << QPointF(area.left(), area.bottom());
        for (std::size_t bin = 0; bin < bins.size(); ++bin) {
            const double x =
                area.left() + (static_cast<double>(bin) * area.width() / (bins.size() - 1));
            const double height =
                static_cast<double>(bins[bin]) * (area.height() - 1) / scopes_.histogram.peak;
            shape << QPointF(x, area.bottom() - height);
        }
        shape << QPointF(area.right(), area.bottom());
        painter.drawPolygon(shape);
    };
    draw(scopes_.histogram.red, kRed);
    draw(scopes_.histogram.green, kGreen);
    draw(scopes_.histogram.blue, kBlue);
    painter.restore();
}

void ScopesPanel::paintVectorscope(QPainter& painter, const QRect& area) const {
    if (!scopes_.vectorscope.isValid()) {
        return;
    }
    // Square, centred: a stretched vectorscope reports a hue shift that is not
    // there.
    const int side = std::min(area.width(), area.height());
    const QRect box(area.left() + ((area.width() - side) / 2),
                    area.top() + ((area.height() - side) / 2), side, side);
    painter.save();
    painter.setClipRect(box);

    painter.setPen(kGraticule);
    painter.drawEllipse(box);
    painter.drawLine(box.center().x(), box.top(), box.center().x(), box.bottom());
    painter.drawLine(box.left(), box.center().y(), box.right(), box.center().y());

    // The colour targets, plotted with the measurement's own arithmetic so the
    // graticule cannot drift away from the trace.
    const struct {
        float r;
        float g;
        float b;
        const char* label;
    } targets[] = {{1, 0, 0, "R"},  {1, 1, 0, "Yl"}, {0, 1, 0, "G"},
                   {0, 1, 1, "Cy"}, {0, 0, 1, "B"},  {1, 0, 1, "Mg"}};
    painter.setPen(kLabel);
    for (const auto& target : targets) {
        float px = 0.0F;
        float py = 0.0F;
        render::Vectorscope::plotFor(target.r, target.g, target.b, side, px, py);
        const QPointF at(box.left() + static_cast<double>(px), box.top() + static_cast<double>(py));
        painter.drawEllipse(at, 3.0, 3.0);
        painter.drawText(at + QPointF(5, -3), target.label);
    }

    const std::uint32_t peak = scopes_.vectorscope.peak();
    const std::int32_t size = scopes_.vectorscope.size();
    for (int y = 0; y < side; ++y) {
        const auto sy = static_cast<std::int32_t>(static_cast<double>(y) * size / side);
        for (int x = 0; x < side; ++x) {
            const auto sx = static_cast<std::int32_t>(static_cast<double>(x) * size / side);
            const float alpha = traceAlpha(scopes_.vectorscope.at(sx, sy), peak);
            if (alpha <= 0.0F) {
                continue;
            }
            QColor pen = kTrace;
            pen.setAlphaF(alpha);
            painter.setPen(pen);
            painter.drawPoint(box.left() + x, box.top() + y);
        }
    }
    painter.restore();
}

}  // namespace zaro::app
