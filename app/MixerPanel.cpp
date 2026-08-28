#include "MixerPanel.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "zaro/core/edit/Operations.h"

#include "Icons.h"
#include "Theme.h"

namespace zaro::app {
namespace {

// The meter's own three colours. Safe reads in the accent, so a level that is
// simply *fine* looks like the rest of the interface; amber and red are kept
// for the two states that are a warning, which is what makes them carry.
const QColor kMeterBack = theme::mix(theme::bg(), Qt::black, 0.35);
const QColor kMeterSafe = theme::accent(500);
const QColor kMeterWarn{226, 196, 92};
const QColor kMeterOver{232, 96, 96};
const QColor kHoldLine = theme::neutral(100);

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
    // Narrow, and capped: a meter is read by where the top of the bar is, and a
    // bar as wide as the strip is a block of colour that dominates a panel
    // whose actual subject is the numbers under it.
    setMaximumWidth(18);
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
    const int filled = static_cast<int>(position * static_cast<float>(area.height()));
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
        const int y = area.bottom() -
                      static_cast<int>(meterPosition(hold_) * static_cast<float>(area.height()));
        painter.setPen(hold_ > 1.0F ? kMeterOver : kHoldLine);
        painter.drawLine(area.left(), y, area.right(), y);
    }
}

MixerPanel::MixerPanel(QWidget* parent) : QWidget{parent} {
    setObjectName("mixer-panel");
    setAttribute(Qt::WA_StyledBackground, true);

    // --- the header the design puts over the console ---------------------
    auto* header = new QFrame(this);
    header->setObjectName("mixer-header");
    header->setFixedHeight(32);
    auto* headerRow = new QHBoxLayout(header);
    headerRow->setContentsMargins(12, 0, 8, 0);
    headerRow->setSpacing(10);
    auto* title = new QLabel("Mixer", header);
    title->setObjectName("gallery-title");
    headerRow->addWidget(title);
    soloLabel_ = new QLabel(header);
    soloLabel_->setProperty("muted", true);
    headerRow->addWidget(soloLabel_);
    headerRow->addStretch(1);

    auto* clear = new QPushButton(header);
    clear->setObjectName("bin-glyph-button");
    clear->setIcon(icons::toolIcon(icons::Glyph::Headphones, 13));
    clear->setFixedSize(26, 24);
    clear->setToolTip("Clear every solo");
    connect(clear, &QPushButton::clicked, this, [this] { clearSolos(); });
    headerRow->addWidget(clear);

    auto* reset = new QPushButton(header);
    reset->setObjectName("bin-glyph-button");
    reset->setIcon(icons::toolIcon(icons::Glyph::Revert, 13));
    reset->setFixedSize(26, 24);
    reset->setToolTip("Faders to unity, pans to centre");
    connect(reset, &QPushButton::clicked, this, [this] { resetFaders(); });
    headerRow->addWidget(reset);

    // --- the console ------------------------------------------------------
    strips_ = new QWidget(this);
    strips_->setObjectName("mixer-console");
    strips_->setAttribute(Qt::WA_StyledBackground, true);
    auto* row = new QHBoxLayout(strips_);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addStretch(1);

    auto* scroll = new QScrollArea(this);
    scroll->setWidget(strips_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(header);
    outer->addWidget(scroll, 1);
}

void MixerPanel::bind(const ui::SequenceBinding& binding) {
    project_ = binding.project;
    sequenceId_ = binding.sequence;
    commands_ = binding.commands;
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
                   [](const AudioStrip* strip, const model::Track& track) {
                       return strip->track() == track.id();
                   });

    if (!sameTracks) {
        for (AudioStrip* strip : strip_) {
            strip->setParent(nullptr);
            strip->deleteLater();
        }
        strip_.clear();
        if (master_ != nullptr) {
            master_->setParent(nullptr);
            master_->deleteLater();
            master_ = nullptr;
        }
        auto* row = qobject_cast<QHBoxLayout*>(strips_->layout());
        if (sequence != nullptr) {
            for (const model::Track& track : sequence->audioTracks()) {
                auto* strip = new AudioStrip{track.id(), AudioStrip::Kind::Track, strips_};
                strip->setObjectName(QString{"mixer-strip-"} + QString::number(track.id().value()));
                row->insertWidget(row->count() - 1, strip);
                connect(strip, &AudioStrip::moved, this,
                        [this, strip](bool committed) { pushState(strip, committed); });
                connect(strip, &AudioStrip::switched, this,
                        [this, strip] { pushState(strip, true); });
                connect(strip, &AudioStrip::picked, this, [this, strip] {
                    picked_ = strip->track();
                    showPicked();
                    emit pickedChanged(picked_);
                });
                strip_.push_back(strip);
            }
            // The master last, on the right, as it is on every console. It is
            // not a track -- there is nothing in the model to write to -- so it
            // carries the summed meter and nothing that can be moved.
            master_ = new AudioStrip{model::TrackId{}, AudioStrip::Kind::Master, strips_};
            master_->setObjectName("mixer-master");
            master_->setName("Master");
            // Before the stretch, so the master sits against the last channel
            // rather than at the far end of an empty console.
            row->insertWidget(row->count() - 1, master_);
        }
        if (!picked_.isValid() && !strip_.empty()) {
            picked_ = strip_.front()->track();
            emit pickedChanged(picked_);
        }
    }

    if (sequence == nullptr) {
        return;
    }
    // Driven from the model, like every other panel: after an undo the strips
    // have to say what the project says, not what they last wrote.
    updating_ = true;
    int soloed = 0;
    for (AudioStrip* strip : strip_) {
        const model::Track* track = sequence->findTrack(strip->track());
        if (track == nullptr) {
            continue;
        }
        strip->setName(QString::fromStdString(track->name()));
        strip->setGainDb(track->gainDb());
        strip->setPan(track->pan());
        strip->setMuted(track->isMuted());
        strip->setSoloed(track->isSoloed());
        strip->setProcessing(track->eq(), track->compressor());
        // A track silenced by someone else's solo is shown dimmed rather than
        // marked muted: it is not muted, and saying so would be a lie the user
        // would then try to undo.
        strip->setEnabled(sequence->isAudible(*track));
        soloed += track->isSoloed() ? 1 : 0;
    }
    updating_ = false;
    showPicked();
    soloLabel_->setText(
        soloed > 0 ? QString("%1 channel%2 solo").arg(soloed).arg(soloed == 1 ? "" : "s")
                   : QString("%1 channel%2").arg(strip_.size()).arg(strip_.size() == 1 ? "" : "s"));
}

void MixerPanel::showPicked() {
    for (AudioStrip* strip : strip_) {
        strip->setPicked(strip->track() == picked_);
    }
}

void MixerPanel::setMeters(const render::AudioGraph::Meters& meters) {
    for (AudioStrip* strip : strip_) {
        strip->setLevel(meters.peakFor(strip->track()));
        const auto found = meters.reduction.find(strip->track().value());
        strip->setReduction(found == meters.reduction.end() ? 0.0F : found->second);
    }
    if (master_ != nullptr) {
        master_->setLevel(meters.masterPeak());
    }
}

void MixerPanel::pushState(AudioStrip* strip, bool committed) {
    if (updating_ || commands_ == nullptr || project_ == nullptr) {
        return;
    }
    edit::TrackState state;
    state.muted = strip->muted();
    state.soloed = strip->soloed();
    state.gainDb = strip->gainDb();
    state.pan = strip->pan();

    auto built = edit::makeSetTrackState(*project_, sequenceId_, strip->track(), state);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    if (committed) {
        commands_->breakMerge();
    }
    // Solo changes what every other strip is doing, so the whole panel re-reads.
    refresh();
    emit edited();
}

void MixerPanel::clearSolos() {
    if (commands_ == nullptr || project_ == nullptr) {
        return;
    }
    const model::Sequence* sequence = project_->findSequence(sequenceId_);
    if (sequence == nullptr) {
        return;
    }
    for (const model::Track& track : sequence->audioTracks()) {
        if (!track.isSoloed()) {
            continue;
        }
        edit::TrackState state;
        state.muted = track.isMuted();
        state.soloed = false;
        state.gainDb = track.gainDb();
        state.pan = track.pan();
        if (auto built = edit::makeSetTrackState(*project_, sequenceId_, track.id(), state)) {
            commands_->execute(*project_, std::move(*built));
        }
    }
    commands_->breakMerge();
    refresh();
    emit edited();
}

void MixerPanel::resetFaders() {
    if (commands_ == nullptr || project_ == nullptr) {
        return;
    }
    const model::Sequence* sequence = project_->findSequence(sequenceId_);
    if (sequence == nullptr) {
        return;
    }
    for (const model::Track& track : sequence->audioTracks()) {
        edit::TrackState state;
        // Mute and solo are left alone: this is a fader reset, and taking a
        // mute off with it would be a second decision nobody asked for.
        state.muted = track.isMuted();
        state.soloed = track.isSoloed();
        state.gainDb = 0.0;
        state.pan = 0.0;
        if (auto built = edit::makeSetTrackState(*project_, sequenceId_, track.id(), state)) {
            commands_->execute(*project_, std::move(*built));
        }
    }
    commands_->breakMerge();
    refresh();
    emit edited();
}

}  // namespace zaro::app
