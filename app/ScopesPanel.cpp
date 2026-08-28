#include "ScopesPanel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "Theme.h"

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
    setObjectName("scopes-panel");

    // A row of chips rather than a drop-down. Four instruments is few enough to
    // show all of them, and a colourist switches between parade and vectorscope
    // constantly -- one click each rather than open, read, pick.
    tabBar_ = new QWidget(this);
    tabBar_->setObjectName("scope-tabs");
    auto* tabRow = new QHBoxLayout(tabBar_);
    tabRow->setContentsMargins(2, 2, 2, 2);
    tabRow->setSpacing(2);
    static constexpr std::pair<const char*, Mode> kTabs[] = {
        {"Parade", Mode::Parade},
        {"Vector", Mode::Vectorscope},
        {"Histogram", Mode::Histogram},
        {"Waveform", Mode::Waveform},
    };
    for (std::size_t at = 0; at < tabs_.size(); ++at) {
        auto* tab = new QPushButton(QString::fromUtf8(kTabs[at].first), tabBar_);
        tab->setObjectName("scope-tab");
        tab->setCheckable(true);
        tab->setFocusPolicy(Qt::NoFocus);
        tab->setCursor(Qt::PointingHandCursor);
        const Mode mode = kTabs[at].second;
        connect(tab, &QPushButton::clicked, this, [this, mode] { setMode(mode); });
        tabRow->addWidget(tab, 1);
        tabs_[at] = tab;
    }

    // Peak, black and saturation: the three numbers somebody checks a shot
    // against, and the three the design puts under the instrument. Read off the
    // same measurement the trace is drawn from, so they cannot disagree with
    // the picture above them.
    readoutRow_ = new QWidget(this);
    auto* readouts = new QHBoxLayout(readoutRow_);
    readouts->setContentsMargins(0, 0, 0, 0);
    readouts->setSpacing(6);
    static constexpr const char* kLabels[] = {"Peak", "Black", "Sat max"};
    static const QColor kInk[] = {QColor{0xd2, 0xce, 0xfd}, QColor{0x8f, 0xc7, 0xd9},
                                  QColor{0xd9, 0xc7, 0x6a}};
    for (std::size_t at = 0; at < values_.size(); ++at) {
        auto* tile = new QWidget(readoutRow_);
        tile->setObjectName("scope-readout");
        tile->setAttribute(Qt::WA_StyledBackground, true);
        auto* column = new QVBoxLayout(tile);
        column->setContentsMargins(8, 5, 8, 5);
        column->setSpacing(1);
        auto* name = new QLabel(QString::fromUtf8(kLabels[at]), tile);
        name->setObjectName("scope-readout-label");
        values_[at] = new QLabel("—", tile);
        values_[at]->setObjectName("scope-readout-value");
        values_[at]->setStyleSheet(QString("color:%1").arg(kInk[at].name()));
        column->addWidget(name);
        column->addWidget(values_[at]);
        readouts->addWidget(tile, 1);
    }

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(tabBar_);
    layout->addStretch(1);
    layout->addWidget(readoutRow_);

    setMode(Mode::Parade);
    setMinimumHeight(160);
}

void ScopesPanel::setMode(Mode mode) {
    mode_ = mode;
    static constexpr Mode kOrder[] = {Mode::Parade, Mode::Vectorscope, Mode::Histogram,
                                      Mode::Waveform};
    for (std::size_t at = 0; at < tabs_.size(); ++at) {
        tabs_[at]->setChecked(kOrder[at] == mode);
    }
    // Every instrument is computed in one pass, so switching does not need a
    // new measurement -- but the panel may have been hidden when the last one
    // was taken.
    if (!hasScopes_) {
        emit measurementNeeded();
    }
    update();
}

/// Peak and black off the luma waveform's own levels, saturation off the
/// vectorscope's furthest occupied cell.
///
/// The waveform is in signal order and 256 levels wide, so a level is a code
/// value and IRE is that as a percentage. Reading the extremes rather than a
/// percentile: what these are for is spotting a clipped highlight or a crushed
/// black, and a percentile is precisely the thing that hides both.
ScopesPanel::Readings ScopesPanel::readings() const {
    Readings found;
    if (!hasScopes_ || !scopes_.histogram.isValid()) {
        return found;
    }
    const auto& luma = scopes_.histogram.luma;
    int lowest = -1;
    int highest = -1;
    for (int level = 0; level < render::Histogram::kBins; ++level) {
        if (luma[static_cast<std::size_t>(level)] == 0) {
            continue;
        }
        highest = level;
        if (lowest < 0) {
            lowest = level;
        }
    }
    if (highest < 0) {
        return found;
    }
    found.peakIre = (highest / 255.0) * 100.0;
    found.blackIre = (lowest / 255.0) * 100.0;

    if (scopes_.vectorscope.isValid()) {
        const std::int32_t size = scopes_.vectorscope.size();
        const double centre = size / 2.0;
        double furthest = 0.0;
        for (std::int32_t y = 0; y < size; ++y) {
            for (std::int32_t x = 0; x < size; ++x) {
                if (scopes_.vectorscope.at(x, y) == 0) {
                    continue;
                }
                furthest = std::max(furthest, std::hypot(x - centre, y - centre));
            }
        }
        found.saturation = std::min(100.0, (furthest / centre) * 100.0);
    }
    return found;
}

void ScopesPanel::showReadings() {
    if (!hasScopes_) {
        for (QLabel* value : values_) {
            value->setText("\u2014");
        }
        return;
    }
    const Readings found = readings();
    values_[0]->setText(QString("%1 IRE").arg(found.peakIre, 0, 'f', 0));
    values_[1]->setText(QString("%1 IRE").arg(found.blackIre, 0, 'f', 1));
    values_[2]->setText(QString("%1%").arg(found.saturation, 0, 'f', 0));
}

bool ScopesPanel::wantsMeasurement() const {
    return isVisible();
}

void ScopesPanel::setScopes(render::FrameScopes scopes) {
    scopes_ = std::move(scopes);
    hasScopes_ = true;
    showReadings();
    update();
}

void ScopesPanel::clear() {
    hasScopes_ = false;
    showReadings();
    update();
}

void ScopesPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // Nothing is measured while the panel is hidden, so it arrives empty.
    emit measurementNeeded();
}

QRect ScopesPanel::plotArea() const {
    return rect().adjusted(4, tabBar_->height() + 8, -4, -(readoutRow_->height() + 8));
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
            const double x = area.left() + (static_cast<double>(bin) * area.width() /
                                            static_cast<double>(bins.size() - 1));
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
