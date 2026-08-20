#include "MixerPanel.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "zaro/core/edit/Operations.h"

namespace zaro::app {
namespace {

const QColor kMeterBack{20, 20, 24};
const QColor kMeterSafe{86, 196, 122};
const QColor kMeterWarn{226, 196, 92};
const QColor kMeterOver{232, 96, 96};
const QColor kHoldLine{240, 240, 248};

/// Where a level sits on the meter, 0 at the bottom and 1 at the top.
///
/// Decibels, not linear: a linear meter spends four fifths of its height on the
/// top two stops and shows nothing at all about a dialogue track sitting at
/// -20 dB, which is where dialogue sits.
float meterPosition(float peak) {
    if (peak <= 0.0F) {
        return 0.0F;
    }
    constexpr float kFloorDb = -60.0F;
    const float db = 20.0F * std::log10(peak);
    return std::clamp((db - kFloorDb) / -kFloorDb, 0.0F, 1.0F);
}

/// How many updates a peak stays up before it starts to fall.
constexpr int kHoldTicks = 12;

}  // namespace

LevelMeter::LevelMeter(QWidget* parent) : QWidget{parent} {
    setMinimumWidth(14);
    setMinimumHeight(80);
}

QSize LevelMeter::sizeHint() const {
    return {16, 120};
}

void LevelMeter::setLevel(float peak) {
    level_ = std::max(0.0F, peak);
    if (level_ >= hold_) {
        hold_ = level_;
        held_ = 0;
    } else if (++held_ > kHoldTicks) {
        // Falls by a fixed fraction per update rather than jumping to the
        // current level: a peak that dropped straight to the signal would only
        // ever be the signal.
        hold_ = std::max(level_, hold_ * 0.82F);
    }
    update();
}

void LevelMeter::resetHold() {
    hold_ = level_;
    held_ = 0;
    update();
}

void LevelMeter::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    const QRect area = rect();
    painter.fillRect(area, kMeterBack);

    const float position = meterPosition(level_);
    const int filled = static_cast<int>(position * area.height());
    if (filled > 0) {
        // Over full scale is red, close to it amber. The boundaries are the
        // ones a mixer's scale marks, not arbitrary fractions of the widget.
        const QColor colour = level_ > 1.0F     ? kMeterOver
                              : level_ > 0.708F ? kMeterWarn  // -3 dBFS
                                                : kMeterSafe;
        painter.fillRect(QRect(area.left(), area.bottom() - filled + 1, area.width(), filled),
                         colour);
    }

    if (hold_ > 0.0F) {
        const int y = area.bottom() - static_cast<int>(meterPosition(hold_) * area.height());
        painter.setPen(hold_ > 1.0F ? kMeterOver : kHoldLine);
        painter.drawLine(area.left(), y, area.right(), y);
    }
}

MixerPanel::MixerPanel(QWidget* parent) : QWidget{parent} {
    strips_ = new QWidget(this);
    auto* row = new QHBoxLayout(strips_);
    row->setContentsMargins(0, 0, 0, 0);

    master_ = new LevelMeter(this);
    master_->setObjectName("mixer-master-meter");

    auto* masterColumn = new QWidget(this);
    auto* masterLayout = new QVBoxLayout(masterColumn);
    masterLayout->setContentsMargins(0, 0, 0, 0);
    auto* masterLabel = new QLabel("Master", this);
    masterLayout->addWidget(masterLabel);
    masterLayout->addWidget(master_, 1);

    auto* outer = new QHBoxLayout(this);
    outer->addWidget(strips_, 1);
    outer->addWidget(masterColumn);
}

void MixerPanel::setProject(model::Project* project, model::SequenceId sequence,
                            edit::CommandStack* commands) {
    project_ = project;
    sequenceId_ = sequence;
    commands_ = commands;
    refresh();
}

void MixerPanel::refresh() {
    const model::Sequence* sequence =
        project_ != nullptr ? project_->findSequence(sequenceId_) : nullptr;

    // Rebuilt only when the set of tracks changes. Tearing the strips down on
    // every refresh would drop whatever the pointer was holding, and a fader
    // that lets go halfway through a drag is unusable.
    const bool sameTracks =
        sequence != nullptr && strip_.size() == sequence->audioTracks().size() &&
        std::equal(strip_.begin(), strip_.end(), sequence->audioTracks().begin(),
                   [](const Strip& strip, const model::Track& track) {
                       return strip.track == track.id();
                   });

    if (!sameTracks) {
        for (const Strip& strip : strip_) {
            strip.name->parentWidget()->deleteLater();
        }
        strip_.clear();
        if (sequence != nullptr) {
            auto* row = qobject_cast<QHBoxLayout*>(strips_->layout());
            for (const model::Track& track : sequence->audioTracks()) {
                Strip strip;
                strip.track = track.id();
                strip.name = new QLabel(QString::fromStdString(track.name()), strips_);
                strip.gain = new QDoubleSpinBox(strips_);
                strip.gain->setRange(-96.0, 12.0);
                strip.gain->setSingleStep(0.5);
                strip.gain->setSuffix(" dB");
                strip.pan = new QDoubleSpinBox(strips_);
                strip.pan->setRange(-1.0, 1.0);
                strip.pan->setSingleStep(0.05);
                strip.pan->setDecimals(2);
                strip.mute = new QCheckBox("M", strips_);
                strip.solo = new QCheckBox("S", strips_);
                strip.meter = new LevelMeter(strips_);
                strip.meter->setObjectName(QString{"mixer-meter-"} +
                                           QString::number(track.id().value()));

                auto* column = new QWidget(strips_);
                auto* layout = new QVBoxLayout(column);
                layout->setContentsMargins(2, 2, 2, 2);
                layout->addWidget(strip.name);
                layout->addWidget(strip.meter, 1);
                auto* buttons = new QWidget(column);
                auto* buttonRow = new QHBoxLayout(buttons);
                buttonRow->setContentsMargins(0, 0, 0, 0);
                buttonRow->addWidget(strip.mute);
                buttonRow->addWidget(strip.solo);
                layout->addWidget(buttons);
                layout->addWidget(strip.gain);
                layout->addWidget(strip.pan);
                row->addWidget(column);

                const Strip captured = strip;
                connect(strip.gain, &QDoubleSpinBox::valueChanged, this,
                        [this, captured] { push(captured); });
                connect(strip.pan, &QDoubleSpinBox::valueChanged, this,
                        [this, captured] { push(captured); });
                connect(strip.mute, &QCheckBox::toggled, this, [this, captured] {
                    push(captured);
                    if (commands_ != nullptr) {
                        commands_->breakMerge();
                    }
                });
                connect(strip.solo, &QCheckBox::toggled, this, [this, captured] {
                    push(captured);
                    if (commands_ != nullptr) {
                        commands_->breakMerge();
                    }
                });
                strip_.push_back(strip);
            }
        }
    }

    if (sequence == nullptr) {
        return;
    }
    // Driven from the model, like every other panel: after an undo the widgets
    // have to say what the project says, not what they last wrote.
    updating_ = true;
    for (const Strip& strip : strip_) {
        const model::Track* track = sequence->findTrack(strip.track);
        if (track == nullptr) {
            continue;
        }
        strip.name->setText(QString::fromStdString(track->name()));
        strip.gain->setValue(track->gainDb());
        strip.pan->setValue(track->pan());
        strip.mute->setChecked(track->isMuted());
        strip.solo->setChecked(track->isSoloed());
        // A track silenced by someone else's solo is shown dimmed rather than
        // marked muted: it is not muted, and saying so would be a lie the user
        // would then try to undo.
        strip.meter->setEnabled(sequence->isAudible(*track));
    }
    updating_ = false;
}

void MixerPanel::setMeters(const render::AudioGraph::Meters& meters) {
    for (const Strip& strip : strip_) {
        strip.meter->setLevel(meters.peakFor(strip.track));
    }
    master_->setLevel(meters.masterPeak());
}

void MixerPanel::push(const Strip& strip) {
    if (updating_ || commands_ == nullptr || project_ == nullptr) {
        return;
    }
    edit::TrackState state;
    state.muted = strip.mute->isChecked();
    state.soloed = strip.solo->isChecked();
    state.gainDb = strip.gain->value();
    state.pan = strip.pan->value();

    auto built = edit::makeSetTrackState(*project_, sequenceId_, strip.track, state);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    // Solo changes what every other strip is doing, so the whole panel re-reads.
    refresh();
    emit edited();
}

}  // namespace zaro::app
