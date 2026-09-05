#include "ColorPalette.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/model/ColorCorrection.h"

#include "ColorWheel.h"
#include "Icons.h"
#include "Theme.h"

namespace zaro::app {
namespace {

// How far a puck at the rim moves each knob. Ranges that make a full deflection
// a strong grade rather than a broken picture: a slope of 1.5 is a bright shot,
// a slope of 4 is a white frame.
constexpr double kOffsetRange = 0.20;
constexpr double kPowerRange = 0.50;
constexpr double kSlopeRange = 0.50;

/// A balance and a master, as three channel deviations.
///
/// The three are 120 degrees apart and sum to zero, so moving the puck changes
/// the colour of a knob without changing its level -- which is what makes a
/// wheel feel like a wheel. The master is what changes the level, and it is
/// added to all three equally.
struct Deviation {
    double r;
    double g;
    double b;
};

Deviation deviationOf(double x, double y, double master) {
    constexpr double kSin120 = 0.8660254037844386;
    return Deviation{x + master, (-0.5 * x) + (kSin120 * y) + master,
                     (-0.5 * x) - (kSin120 * y) + master};
}

/// The inverse: read a puck position back out of three channel values.
///
/// Needed because the panel is driven from the model rather than from its own
/// widgets -- after an undo, the wheels have to be able to show what the clip
/// now says, and that means turning three numbers back into a point.
void balanceOf(const Deviation& deviation, double& x, double& y, double& master) {
    constexpr double kSqrt3 = 1.7320508075688772;
    master = (deviation.r + deviation.g + deviation.b) / 3.0;
    const double r = deviation.r - master;
    const double g = deviation.g - master;
    const double b = deviation.b - master;
    x = r;
    y = (g - b) / kSqrt3;
}

/// 0..1 across a range, and back. The sliders are all fractions; what the
/// fraction means is written once, here.
double toFraction(double value, double low, double high) {
    return std::clamp((value - low) / (high - low), 0.0, 1.0);
}
double fromFraction(double fraction, double low, double high) {
    return low + (fraction * (high - low));
}

}  // namespace

// --- GradientSlider -------------------------------------------------------

GradientSlider::GradientSlider(QString label, QColor from, QColor middle, QColor to,
                               QWidget* parent)
    : QWidget{parent}, label_{std::move(label)}, from_{from}, middle_{middle}, to_{to} {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setToolTip(label_ + " — double-click to reset");
}

QSize GradientSlider::sizeHint() const {
    return QSize{190, 24};
}

QRect GradientSlider::trackRect() const {
    return QRect{0, height() - 6, width(), 4};
}

void GradientSlider::setFraction(double fraction) {
    fraction_ = std::clamp(fraction, 0.0, 1.0);
    update();
}

void GradientSlider::setReadout(const QString& text) {
    readout_ = text;
    update();
}

void GradientSlider::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);

    QFont label = font();
    label.setPointSizeF(8.0);
    painter.setFont(label);
    painter.setPen(theme::textAt(0.55));
    const QRect row{0, 0, width(), height() - 9};
    painter.drawText(row, Qt::AlignLeft | Qt::AlignVCenter, label_);

    QFont readout{QStringLiteral("Menlo")};
    readout.setStyleHint(QFont::Monospace);
    readout.setPointSizeF(7.5);
    painter.setFont(readout);
    painter.drawText(row, Qt::AlignRight | Qt::AlignVCenter, readout_);

    const QRect track = trackRect();
    QLinearGradient ramp{QPointF{static_cast<double>(track.left()), 0.0},
                         QPointF{static_cast<double>(track.right()), 0.0}};
    ramp.setColorAt(0.0, from_);
    if (middle_.isValid()) {
        ramp.setColorAt(0.5, middle_);
    }
    ramp.setColorAt(1.0, to_);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ramp);
    painter.drawRoundedRect(track, 2, 2);

    const double at = track.left() + (fraction_ * track.width());
    painter.setBrush(theme::text());
    painter.setPen(QPen{QColor{0, 0, 0, 150}, 1.0});
    painter.drawEllipse(QPointF{at, track.center().y() + 0.5}, 5.0, 5.0);
}

void GradientSlider::take(const QPoint& where) {
    const QRect track = trackRect();
    setFraction(track.width() > 0 ? static_cast<double>(where.x() - track.left()) / track.width()
                                  : 0.0);
}

void GradientSlider::mousePressEvent(QMouseEvent* event) {
    dragging_ = true;
    take(event->pos());
    emit changed(false);
}

void GradientSlider::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) {
        return;
    }
    take(event->pos());
    emit changed(false);
}

void GradientSlider::mouseReleaseEvent(QMouseEvent* /*event*/) {
    if (!dragging_) {
        return;
    }
    dragging_ = false;
    emit changed(true);
}

void GradientSlider::mouseDoubleClickEvent(QMouseEvent* /*event*/) {
    setFraction(neutral_);
    emit changed(true);
}

// --- ColorPalette ---------------------------------------------------------

ColorPalette::ColorPalette(QWidget* parent) : QWidget{parent} {
    setObjectName("color-palette");
    setAttribute(Qt::WA_StyledBackground, true);

    // The palette list. Two entries rather than the design's five: Wheels and
    // Bars are two ways of typing the same ASC CDL, which this project has, and
    // Log, Curves and Blur name controls it does not -- a log wheel is a
    // different parameterisation, and a blur is not a colour decision.
    palettes_ = new QListWidget(this);
    palettes_->setObjectName("palette-list");
    palettes_->setFrameShape(QFrame::NoFrame);
    palettes_->setFixedWidth(150);
    for (const auto& [name, glyph] : {std::pair{QStringLiteral("Wheels"), icons::Glyph::Circle},
                                      std::pair{QStringLiteral("Bars"), icons::Glyph::Rows}}) {
        auto* item = new QListWidgetItem(name, palettes_);
        item->setIcon(icons::toolIcon(glyph, 14));
    }
    palettes_->setCurrentRow(0);

    // --- wheels ---------------------------------------------------------
    auto* wheelRow = new QWidget(this);
    auto* wheelLayout = new QHBoxLayout(wheelRow);
    wheelLayout->setContentsMargins(24, 0, 0, 0);
    wheelLayout->setSpacing(26);
    static constexpr const char* kWheelNames[] = {"Lift", "Gamma", "Gain"};
    for (std::size_t at = 0; at < wheels_.size(); ++at) {
        wheels_[at] = new ColorWheel{QString::fromUtf8(kWheelNames[at]), wheelRow};
        wheels_[at]->setObjectName(QString("wheel-%1").arg(QString::fromUtf8(kWheelNames[at])));
        wheelLayout->addWidget(wheels_[at]);
        connect(wheels_[at], &ColorWheel::changed, this,
                [this](bool committed) { pushWheels(committed); });
    }
    wheelLayout->addStretch(1);

    // --- bars: the same nine numbers, typed one channel at a time --------
    auto* barRow = new QWidget(this);
    auto* barLayout = new QHBoxLayout(barRow);
    barLayout->setContentsMargins(24, 6, 0, 6);
    barLayout->setSpacing(22);
    static constexpr const char* kChannels[] = {"R", "G", "B"};
    for (int knob = 0; knob < 3; ++knob) {
        auto* column = new QWidget(barRow);
        auto* columnLayout = new QVBoxLayout(column);
        columnLayout->setContentsMargins(0, 0, 0, 0);
        columnLayout->setSpacing(2);
        auto* heading = new QLabel(QString::fromUtf8(kWheelNames[knob]), column);
        heading->setProperty("muted", true);
        columnLayout->addWidget(heading);
        for (int channel = 0; channel < 3; ++channel) {
            // The track is the channel it moves, so a column of three reads as
            // red, green, blue without three more labels to say so.
            static const QColor kInk[] = {QColor{0xd9, 0x6a, 0x6a}, QColor{0x7f, 0xd9, 0x8f},
                                          QColor{0x7f, 0x8f, 0xd9}};
            auto* bar = new GradientSlider{QString::fromUtf8(kChannels[channel]),
                                           theme::neutral(800), QColor{}, kInk[channel], column};
            bar->setFixedWidth(150);
            bars_.push_back(bar);
            columnLayout->addWidget(bar);
            connect(bar, &GradientSlider::changed, this,
                    [this](bool committed) { pushWheels(committed); });
        }
        barLayout->addWidget(column);
    }
    barLayout->addStretch(1);

    pages_ = new QStackedWidget(this);
    pages_->addWidget(wheelRow);
    pages_->addWidget(barRow);
    connect(palettes_, &QListWidget::currentRowChanged, this,
            [this](int row) { pages_->setCurrentIndex(std::max(0, row)); });

    // --- temperature, tint, saturation -----------------------------------
    temperature_ = new GradientSlider{"Temperature", QColor{0x6a, 0x8f, 0xd9}, theme::neutral(600),
                                      QColor{0xd9, 0xa8, 0x6a}, this};
    tint_ = new GradientSlider{"Tint", QColor{0x7f, 0xd9, 0x8f}, theme::neutral(600),
                               QColor{0xd9, 0x6a, 0xc7}, this};
    saturation_ = new GradientSlider{"Saturation", theme::neutral(700), QColor{},
                                     QColor{0xd9, 0x6a, 0xc7}, this};
    // Saturation's neutral is a third of the way along, because the scale runs
    // 0..200 with 100 untouched -- not the middle of the track.
    saturation_->setNeutral(0.5);
    for (GradientSlider* slider : {temperature_, tint_, saturation_}) {
        slider->setFixedWidth(190);
        connect(slider, &GradientSlider::changed, this,
                [this](bool committed) { pushCorrection(committed); });
    }
    auto* ramps = new QWidget(this);
    auto* rampColumn = new QVBoxLayout(ramps);
    rampColumn->setContentsMargins(0, 0, 24, 0);
    rampColumn->setSpacing(8);
    rampColumn->addStretch(1);
    rampColumn->addWidget(temperature_);
    rampColumn->addWidget(tint_);
    rampColumn->addWidget(saturation_);
    rampColumn->addStretch(1);

    // The strip above, not the timeline: the Color workspace hides the timeline
    // entirely -- see `PreviewWindow::setWorkspace` -- so this was telling
    // somebody to use a panel that is not on the screen while they read it.
    empty_ = new QLabel("Select a shot in the strip above to grade it", this);
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setProperty("muted", true);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(palettes_);
    row->addWidget(pages_, 1);
    row->addWidget(ramps);
    row->addWidget(empty_, 1);

    refresh();
}

void ColorPalette::bind(const ui::SequenceBinding& binding) {
    project_ = binding.project;
    sequenceId_ = binding.sequence;
    commands_ = binding.commands;
    refresh();
}

void ColorPalette::setSelection(model::TrackId track, model::ClipId clip) {
    track_ = track;
    clip_ = clip;
    refresh();
}

const model::Clip* ColorPalette::selectedClip() const {
    if (project_ == nullptr || !clip_.isValid()) {
        return nullptr;
    }
    const model::Sequence* sequence = project_->findSequence(sequenceId_);
    if (sequence == nullptr) {
        return nullptr;
    }
    const model::Track* track = sequence->findTrack(track_);
    return track != nullptr ? track->find(clip_) : nullptr;
}

void ColorPalette::refresh() {
    const model::Clip* clip = selectedClip();
    const bool have = clip != nullptr;
    palettes_->setVisible(have);
    pages_->setVisible(have);
    temperature_->parentWidget()->setVisible(have);
    empty_->setVisible(!have);
    if (!have) {
        return;
    }

    // Written from the model, not remembered: after an undo the clip is the
    // only thing that knows what the grade is.
    updating_ = true;
    const model::ColorWheels& wheels = clip->wheels;
    const Deviation knobs[] = {
        {wheels.offsetR, wheels.offsetG, wheels.offsetB},
        {wheels.powerR - 1.0, wheels.powerG - 1.0, wheels.powerB - 1.0},
        {wheels.slopeR - 1.0, wheels.slopeG - 1.0, wheels.slopeB - 1.0},
    };
    const double ranges[] = {kOffsetRange, kPowerRange, kSlopeRange};
    for (std::size_t at = 0; at < wheels_.size(); ++at) {
        const Deviation scaled{knobs[at].r / ranges[at], knobs[at].g / ranges[at],
                               knobs[at].b / ranges[at]};
        double x = 0.0;
        double y = 0.0;
        double master = 0.0;
        balanceOf(scaled, x, y, master);
        wheels_[at]->setBalance(x, y);
        wheels_[at]->setMaster(master);
        for (int channel = 0; channel < 3; ++channel) {
            const double value = channel == 0 ? scaled.r : (channel == 1 ? scaled.g : scaled.b);
            bars_[(at * 3) + static_cast<std::size_t>(channel)]->setFraction(
                toFraction(value, -1.0, 1.0));
            bars_[(at * 3) + static_cast<std::size_t>(channel)]->setReadout(
                QString::number(value, 'f', 2));
        }
    }

    const model::ColorCorrection& colour = clip->color;
    temperature_->setFraction(toFraction(colour.temperature, -100.0, 100.0));
    temperature_->setReadout(QString::number(colour.temperature, 'f', 0));
    tint_->setFraction(toFraction(colour.tint, -100.0, 100.0));
    tint_->setReadout(QString::number(colour.tint, 'f', 0));
    saturation_->setFraction(toFraction(colour.saturation, 0.0, 200.0));
    saturation_->setReadout(QString::number(colour.saturation, 'f', 0));
    updating_ = false;
}

void ColorPalette::pushWheels(bool committed) {
    if (updating_ || commands_ == nullptr || project_ == nullptr || !clip_.isValid()) {
        return;
    }
    // Which control moved decides which reading is authoritative: the wheels
    // and the bars are the same nine numbers, and reading the page that is not
    // on screen would throw away what somebody just did.
    const bool fromBars = pages_->currentIndex() == 1;
    model::ColorWheels wheels;
    double* const channels[3][3] = {
        {&wheels.offsetR, &wheels.offsetG, &wheels.offsetB},
        {&wheels.powerR, &wheels.powerG, &wheels.powerB},
        {&wheels.slopeR, &wheels.slopeG, &wheels.slopeB},
    };
    const double ranges[] = {kOffsetRange, kPowerRange, kSlopeRange};
    const double neutral[] = {0.0, 1.0, 1.0};
    for (std::size_t knob = 0; knob < 3; ++knob) {
        Deviation scaled{0.0, 0.0, 0.0};
        if (fromBars) {
            scaled.r = fromFraction(bars_[knob * 3]->fraction(), -1.0, 1.0);
            scaled.g = fromFraction(bars_[(knob * 3) + 1]->fraction(), -1.0, 1.0);
            scaled.b = fromFraction(bars_[(knob * 3) + 2]->fraction(), -1.0, 1.0);
        } else {
            scaled = deviationOf(wheels_[knob]->balanceX(), wheels_[knob]->balanceY(),
                                 wheels_[knob]->master());
        }
        // A power or a slope of zero is a black frame that no further move can
        // recover from, so the floor is small rather than nothing.
        const double floor = knob == 0 ? -1.0 : 0.01;
        *channels[knob][0] = std::max(floor, neutral[knob] + (scaled.r * ranges[knob]));
        *channels[knob][1] = std::max(floor, neutral[knob] + (scaled.g * ranges[knob]));
        *channels[knob][2] = std::max(floor, neutral[knob] + (scaled.b * ranges[knob]));
    }

    auto built = edit::makeSetWheels(*project_, {sequenceId_, track_}, clip_, wheels);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    if (committed) {
        // One undo step per gesture: the merge is broken when the hand comes
        // off, not on every pixel of the drag.
        commands_->breakMerge();
    }
    refresh();
    emit edited();
}

void ColorPalette::pushCorrection(bool committed) {
    if (updating_ || commands_ == nullptr || project_ == nullptr || !clip_.isValid()) {
        return;
    }
    const model::Clip* clip = selectedClip();
    if (clip == nullptr) {
        return;
    }
    // Exposure and contrast are not on this bar, so they are carried through
    // rather than defaulted -- a temperature drag must not silently flatten a
    // clip's exposure.
    model::ColorCorrection colour = clip->color;
    colour.temperature = fromFraction(temperature_->fraction(), -100.0, 100.0);
    colour.tint = fromFraction(tint_->fraction(), -100.0, 100.0);
    colour.saturation = fromFraction(saturation_->fraction(), 0.0, 200.0);

    auto built = edit::makeSetColorCorrection(*project_, {sequenceId_, track_}, clip_, colour);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    if (committed) {
        commands_->breakMerge();
    }
    refresh();
    emit edited();
}

void ColorPalette::resetGrade() {
    if (commands_ == nullptr || project_ == nullptr || !clip_.isValid()) {
        return;
    }
    if (auto built =
            edit::makeSetWheels(*project_, {sequenceId_, track_}, clip_, model::ColorWheels{})) {
        commands_->execute(*project_, std::move(*built));
    }
    if (auto built = edit::makeSetColorCorrection(*project_, {sequenceId_, track_}, clip_,
                                                  model::ColorCorrection{})) {
        commands_->execute(*project_, std::move(*built));
    }
    commands_->breakMerge();
    refresh();
    emit edited();
}

}  // namespace zaro::app
