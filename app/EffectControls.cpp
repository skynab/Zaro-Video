#include "EffectControls.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

#include "zaro/core/edit/Operations.h"

namespace zaro::app {
namespace {

QDoubleSpinBox* makeSpin(double minimum, double maximum, double step, int decimals,
                         const QString& suffix = {}) {
    auto* spin = new QDoubleSpinBox;
    spin->setRange(minimum, maximum);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    if (!suffix.isEmpty()) {
        spin->setSuffix(suffix);
    }
    // Keyboard focus only on click, so scrolling the panel does not silently
    // change whatever value happens to be under the pointer.
    spin->setFocusPolicy(Qt::StrongFocus);
    return spin;
}

}  // namespace

EffectControls::EffectControls(QWidget* parent) : QWidget{parent} {
    title_ = new QLabel("No clip selected", this);
    title_->setStyleSheet("font-weight: 600;");

    positionX_ = makeSpin(-100000.0, 100000.0, 1.0, 1, " px");
    positionY_ = makeSpin(-100000.0, 100000.0, 1.0, 1, " px");
    scaleX_ = makeSpin(-100.0, 100.0, 0.01, 3);
    scaleY_ = makeSpin(-100.0, 100.0, 0.01, 3);
    rotation_ = makeSpin(-3600.0, 3600.0, 1.0, 2, QString::fromUtf8("°"));
    anchorX_ = makeSpin(-100000.0, 100000.0, 1.0, 1, " px");
    anchorY_ = makeSpin(-100000.0, 100000.0, 1.0, 1, " px");
    opacity_ = makeSpin(0.0, 1.0, 0.01, 3);

    blend_ = new QComboBox(this);
    for (const model::BlendMode mode : {model::BlendMode::Normal, model::BlendMode::Add,
                                        model::BlendMode::Multiply, model::BlendMode::Screen}) {
        blend_->addItem(QString::fromUtf8(model::toString(mode)), static_cast<int>(mode));
    }

    gain_ = makeSpin(-96.0, 24.0, 0.5, 2, " dB");
    pan_ = makeSpin(-1.0, 1.0, 0.05, 3);
    enabled_ = new QCheckBox("Enabled", this);

    auto* motion = new QGroupBox("Motion", this);
    auto* motionForm = new QFormLayout(motion);
    motionForm->addRow("Position X", positionX_);
    motionForm->addRow("Position Y", positionY_);
    motionForm->addRow("Scale X", scaleX_);
    motionForm->addRow("Scale Y", scaleY_);
    motionForm->addRow("Rotation", rotation_);
    motionForm->addRow("Anchor X", anchorX_);
    motionForm->addRow("Anchor Y", anchorY_);
    motionForm->addRow("Opacity", opacity_);
    motionForm->addRow("Blend", blend_);
    videoGroup_ = motion;

    auto* audio = new QGroupBox("Audio", this);
    auto* audioForm = new QFormLayout(audio);
    audioForm->addRow("Gain", gain_);
    audioForm->addRow("Pan", pan_);
    audioGroup_ = audio;

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(title_);
    layout->addWidget(enabled_);
    layout->addWidget(motion);
    layout->addWidget(audio);
    layout->addStretch(1);

    for (QDoubleSpinBox* spin :
         {positionX_, positionY_, scaleX_, scaleY_, rotation_, anchorX_, anchorY_, opacity_}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { pushTransform(); });
    }
    for (QDoubleSpinBox* spin : {gain_, pan_}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { pushAudio(); });
    }
    connect(blend_, &QComboBox::currentIndexChanged, this, [this] {
        if (updating_ || commands_ == nullptr || !clip_.isValid()) {
            return;
        }
        const auto mode = static_cast<model::BlendMode>(blend_->currentData().toInt());
        auto built = edit::makeSetBlendMode(*project_, {sequenceId_, track_}, clip_, mode);
        if (built) {
            commands_->execute(*project_, std::move(*built));
            commands_->breakMerge();
            emit edited();
        }
    });
    connect(enabled_, &QCheckBox::toggled, this, [this](bool on) {
        if (updating_ || commands_ == nullptr || !clip_.isValid()) {
            return;
        }
        auto built = edit::makeSetClipEnabled(*project_, {sequenceId_, track_}, clip_, on);
        if (built) {
            commands_->execute(*project_, std::move(*built));
            emit edited();
        }
    });

    setEditingEnabled(false);
}

void EffectControls::setProject(model::Project* project, model::SequenceId sequence,
                                edit::CommandStack* commands) {
    project_ = project;
    sequenceId_ = sequence;
    commands_ = commands;
    setSelection({}, {});
}

void EffectControls::setSelection(model::TrackId track, model::ClipId clip) {
    track_ = track;
    clip_ = clip;
    refresh();
}

const model::Clip* EffectControls::selectedClip() const {
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

void EffectControls::setEditingEnabled(bool enabled) {
    enabled_->setEnabled(enabled);
    videoGroup_->setEnabled(enabled);
    audioGroup_->setEnabled(enabled);
}

void EffectControls::refresh() {
    applyToWidgets();
}

void EffectControls::applyToWidgets() {
    const model::Clip* clip = selectedClip();
    if (clip == nullptr) {
        title_->setText("No clip selected");
        setEditingEnabled(false);
        // Show the identity transform rather than whatever was last selected,
        // and never leave zeros in Scale or Opacity: those are meaningful
        // values, and a disabled panel displaying them reads as a clip that is
        // scaled to nothing rather than as no clip at all.
        updating_ = true;
        const model::Transform identity;
        positionX_->setValue(identity.positionX);
        positionY_->setValue(identity.positionY);
        scaleX_->setValue(identity.scaleX);
        scaleY_->setValue(identity.scaleY);
        rotation_->setValue(identity.rotationDegrees);
        anchorX_->setValue(identity.anchorX);
        anchorY_->setValue(identity.anchorY);
        opacity_->setValue(identity.opacity);
        blend_->setCurrentIndex(blend_->findData(static_cast<int>(model::BlendMode::Normal)));
        gain_->setValue(0.0);
        pan_->setValue(0.0);
        enabled_->setChecked(false);
        updating_ = false;
        return;
    }

    const model::Sequence* sequence = project_->findSequence(sequenceId_);
    const model::Track* track = sequence->findTrack(track_);
    const bool isVideo = track != nullptr && track->kind() == model::TrackKind::Video;

    title_->setText(QString::fromStdString(clip->name.empty() ? "Clip" : clip->name));
    setEditingEnabled(true);
    // Motion applies to picture, gain and pan to sound. Showing both for every
    // clip would offer controls that do nothing.
    videoGroup_->setVisible(isVideo);
    audioGroup_->setVisible(!isVideo);
    if (track != nullptr && track->isLocked()) {
        setEditingEnabled(false);
        title_->setText(title_->text() + "  (track locked)");
    }

    // Guarded, so writing the model's values into the widgets does not read as
    // the user changing them and bounce straight back.
    updating_ = true;
    const model::Transform& transform = clip->transform;
    positionX_->setValue(transform.positionX);
    positionY_->setValue(transform.positionY);
    scaleX_->setValue(transform.scaleX);
    scaleY_->setValue(transform.scaleY);
    rotation_->setValue(transform.rotationDegrees);
    anchorX_->setValue(transform.anchorX);
    anchorY_->setValue(transform.anchorY);
    opacity_->setValue(transform.opacity);
    blend_->setCurrentIndex(blend_->findData(static_cast<int>(clip->blend)));
    gain_->setValue(clip->gainDb);
    pan_->setValue(clip->pan);
    enabled_->setChecked(clip->enabled);
    updating_ = false;
}

void EffectControls::pushTransform() {
    if (updating_ || commands_ == nullptr || !clip_.isValid()) {
        return;
    }
    model::Transform transform;
    transform.positionX = positionX_->value();
    transform.positionY = positionY_->value();
    transform.scaleX = scaleX_->value();
    transform.scaleY = scaleY_->value();
    transform.rotationDegrees = rotation_->value();
    transform.anchorX = anchorX_->value();
    transform.anchorY = anchorY_->value();
    transform.opacity = opacity_->value();

    auto built = edit::makeSetTransform(*project_, {sequenceId_, track_}, clip_, transform);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    emit edited();
}

void EffectControls::pushAudio() {
    if (updating_ || commands_ == nullptr || !clip_.isValid()) {
        return;
    }
    auto built = edit::makeSetClipAudio(*project_, {sequenceId_, track_}, clip_, gain_->value(),
                                        pan_->value());
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    emit edited();
}

}  // namespace zaro::app
