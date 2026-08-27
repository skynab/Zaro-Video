#include "LoudnessPanel.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "Theme.h"

namespace zaro::app {
namespace {

// The scale the bar is drawn on: -40 LUFS at the bottom, 0 at the top.
constexpr double kFloor = -40.0;
constexpr double kCeiling = 0.0;
constexpr int kBarWidth = 64;
constexpr int kBarHeight = 104;
constexpr int kTop = 30;

double barFraction(double lufs) {
    return std::clamp((lufs - kFloor) / (kCeiling - kFloor), 0.0, 1.0);
}

/// A rough momentary figure from a linear peak.
///
/// **Not a BS.1770 measurement**, and labelled as an approximation for that
/// reason: the real thing needs K-weighted filtering over a 400 ms window,
/// which is what `LoudnessMeter` does when the sequence is measured properly.
/// What this is for is a bar that moves with the mix while somebody is working.
double roughLufs(float peak) {
    if (peak <= 0.004F) {
        return kFloor;
    }
    return std::clamp(20.0 * std::log10(static_cast<double>(peak)) - 3.0, kFloor, kCeiling);
}

}  // namespace

LoudnessPanel::LoudnessPanel(QWidget* parent) : QWidget{parent} {
    setObjectName("loudness-panel");
    setAttribute(Qt::WA_StyledBackground, true);
    // Room for the bar's caption and the button under it: at 34 the caption
    // and the button were drawn on top of each other.
    setFixedHeight(kTop + kBarHeight + 52);

    measure_ = new QPushButton("Measure", this);
    measure_->setObjectName("loudness-measure");
    measure_->setToolTip("Mix the whole sequence and take the integrated reading");
    measure_->setFixedHeight(20);
    connect(measure_, &QPushButton::clicked, this, [this] { emit measureRequested(); });

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(12, 6, 12, 10);
    column->addStretch(1);
    column->addWidget(measure_);
}

void LoudnessPanel::setMeasurement(const render::AudioGraph::LoudnessResult& result) {
    result_ = result;
    measured_ = true;
    update();
}

void LoudnessPanel::clearMeasurement() {
    measured_ = false;
    update();
}

void LoudnessPanel::setMasterPeak(float peak) {
    masterPeak_ = peak;
    update();
}

QRect LoudnessPanel::barRect() const {
    return QRect{12, kTop, kBarWidth, kBarHeight};
}

void LoudnessPanel::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);

    QFont heading = font();
    heading.setPointSizeF(7.5);
    heading.setCapitalization(QFont::AllUppercase);
    heading.setLetterSpacing(QFont::PercentageSpacing, 108);
    painter.setFont(heading);
    painter.setPen(theme::textAt(0.38));
    painter.drawText(QRect{12, 6, width() - 24, 16}, Qt::AlignLeft | Qt::AlignVCenter, "Loudness");

    // The verdict, which is the whole point of the panel: on target, over, or
    // under. A number without it is a number somebody still has to judge.
    QFont verdictFont = font();
    verdictFont.setPointSizeF(7.5);
    painter.setFont(verdictFont);
    const double integrated = result_.integratedLufs;
    const bool onTarget = measured_ && std::abs(integrated - target_) < 1.0;
    const QColor good{0x8f, 0xd9, 0xa8};
    const QColor warn{0xd9, 0xc7, 0x6a};
    painter.setPen(!measured_ ? theme::textAt(0.35) : (onTarget ? good : warn));
    painter.drawText(QRect{12, 6, width() - 24, 16}, Qt::AlignRight | Qt::AlignVCenter,
                     !measured_        ? QStringLiteral("Not measured")
                     : onTarget        ? QStringLiteral("On target")
                     : integrated > target_ ? QStringLiteral("Over target")
                                            : QStringLiteral("Under target"));

    // --- the moving bar ---------------------------------------------------
    const QRect bar = barRect();
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::well());
    painter.drawRoundedRect(bar, 5, 5);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen{theme::divider(), 1.0});
    painter.drawRoundedRect(QRectF{bar}.adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);

    const int targetY = bar.bottom() - static_cast<int>(barFraction(target_) * bar.height());
    painter.setPen(QPen{warn, 1.0});
    painter.drawLine(bar.left() + 1, targetY, bar.right() - 1, targetY);

    const double momentary = roughLufs(masterPeak_);
    const int tall = static_cast<int>(barFraction(momentary) * (bar.height() - 2));
    if (tall > 0) {
        const QRect lit{bar.left() + 6, bar.bottom() - tall, bar.width() - 12, tall};
        QLinearGradient wash{QPointF{0, static_cast<double>(lit.bottom())},
                             QPointF{0, static_cast<double>(lit.top())}};
        wash.setColorAt(0.0, QColor{0x6a, 0xc7, 0xd9});
        wash.setColorAt(1.0, QColor{0x8f, 0xd9, 0xe6});
        painter.setPen(Qt::NoPen);
        painter.setBrush(wash);
        painter.drawRoundedRect(lit, 3, 3);
    }
    QFont tiny = font();
    tiny.setPointSizeF(6.5);
    painter.setFont(tiny);
    painter.setPen(theme::textAt(0.38));
    painter.drawText(QRect{bar.left(), bar.bottom() + 2, bar.width(), 12}, Qt::AlignCenter,
                     "Momentary");

    // --- the numbers ------------------------------------------------------
    QFont label = font();
    label.setPointSizeF(8.0);
    QFont mono{QStringLiteral("Menlo")};
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSizeF(8.5);

    const struct {
        QString label;
        QString value;
        QColor ink;
    } rows[] = {
        {QStringLiteral("Integrated"),
         measured_ ? QString("%1 LUFS").arg(integrated, 0, 'f', 1) : QStringLiteral("—"),
         !measured_ ? theme::textAt(0.40) : (onTarget ? good : warn)},
        {QStringLiteral("To target"),
         measured_ ? QString("%1 dB").arg(result_.gainToReach(target_), 0, 'f', 1)
                   : QStringLiteral("—"),
         theme::textAt(0.72)},
        {QStringLiteral("Sample peak"),
         measured_ ? QString("%1 dBFS").arg(result_.samplePeakDbfs, 0, 'f', 1)
                   : QStringLiteral("—"),
         QColor{0x8f, 0xc7, 0xd9}},
        {QStringLiteral("Target"), QString("%1 LUFS").arg(target_, 0, 'f', 0),
         theme::textAt(0.50)},
    };

    const int left = bar.right() + 12;
    const int rowHeight = 22;
    int y = bar.top() + 6;
    for (const auto& row : rows) {
        painter.setFont(label);
        painter.setPen(theme::textAt(0.52));
        painter.drawText(QRect{left, y, width() - left - 12, rowHeight},
                         Qt::AlignLeft | Qt::AlignVCenter, row.label);
        painter.setFont(mono);
        painter.setPen(row.ink);
        painter.drawText(QRect{left, y, width() - left - 12, rowHeight},
                         Qt::AlignRight | Qt::AlignVCenter, row.value);
        y += rowHeight;
    }
}

}  // namespace zaro::app
