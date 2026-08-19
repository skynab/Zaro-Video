#include "EffectControls.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/model/ColorCorrection.h"

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

    // Wide enough for the largest value it can hold, with its suffix. A field
    // that shrinks with the panel eventually cuts the unit off, and "0.00 E"
    // reads as a different number rather than as a shorter label -- which has
    // now happened twice, once to " stops" and once to " EV".
    QString widest = QString::number(minimum, 'f', decimals) + suffix;
    if (QString::number(maximum, 'f', decimals).size() > widest.size()) {
        widest = QString::number(maximum, 'f', decimals) + suffix;
    }
    spin->setMinimumWidth(spin->fontMetrics().horizontalAdvance(widest) + 34);
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

    // The units a panel shows, not the ones the arithmetic wants: stops for
    // exposure, -100..100 for the rest, 100 for neutral saturation.
    temperature_ = makeSpin(-100.0, 100.0, 1.0, 1);
    tint_ = makeSpin(-100.0, 100.0, 1.0, 1);
    // "EV" rather than "stops": the same unit, and it fits in the field. A
    // suffix that gets cut off reads as a different number, not as a shorter
    // label.
    exposure_ = makeSpin(-6.0, 6.0, 0.1, 2, " EV");
    contrast_ = makeSpin(-100.0, 100.0, 1.0, 1);
    saturation_ = makeSpin(0.0, 200.0, 1.0, 1);

    gain_ = makeSpin(-96.0, 24.0, 0.5, 2, " dB");
    pan_ = makeSpin(-1.0, 1.0, 0.05, 3);
    enabled_ = new QCheckBox("Enabled", this);

    auto* motion = new QGroupBox("Motion", this);
    auto* motionForm = new QFormLayout(motion);
    addRow(motionForm, "Position X", model::Param::PositionX, positionX_);
    addRow(motionForm, "Position Y", model::Param::PositionY, positionY_);
    addRow(motionForm, "Scale X", model::Param::ScaleX, scaleX_);
    addRow(motionForm, "Scale Y", model::Param::ScaleY, scaleY_);
    addRow(motionForm, "Rotation", model::Param::RotationDegrees, rotation_);
    addRow(motionForm, "Anchor X", model::Param::AnchorX, anchorX_);
    addRow(motionForm, "Anchor Y", model::Param::AnchorY, anchorY_);
    addRow(motionForm, "Opacity", model::Param::Opacity, opacity_);
    // Blend has no stopwatch: it is a mode rather than a quantity, and there is
    // no meaningful value halfway between Multiply and Screen.
    motionForm->addRow("Blend", blend_);
    videoGroup_ = motion;

    auto* colour = new QGroupBox("Colour", this);
    auto* colourForm = new QFormLayout(colour);
    addRow(colourForm, "Temperature", model::Param::Temperature, temperature_);
    addRow(colourForm, "Tint", model::Param::Tint, tint_);
    addRow(colourForm, "Exposure", model::Param::Exposure, exposure_);
    addRow(colourForm, "Contrast", model::Param::Contrast, contrast_);
    addRow(colourForm, "Saturation", model::Param::Saturation, saturation_);
    curves_ = new CurveEditor(this);
    colourForm->addRow(curves_);
    connect(curves_, &CurveEditor::curvesChanged, this,
            [this](const model::ToneCurves& changed, bool committed) {
                pushCurves(changed, committed);
            });
    colorGroup_ = colour;

    auto* audio = new QGroupBox("Audio", this);
    auto* audioForm = new QFormLayout(audio);
    addRow(audioForm, "Gain", model::Param::GainDb, gain_);
    addRow(audioForm, "Pan", model::Param::Pan, pan_);
    audioGroup_ = audio;

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(title_);
    layout->addWidget(enabled_);
    layout->addWidget(motion);
    layout->addWidget(colour);
    layout->addWidget(audio);
    layout->addStretch(1);

    for (const Row& row : rows_) {
        const model::Param param = row.param;
        connect(row.spin, &QDoubleSpinBox::valueChanged, this,
                [this, param](double value) { pushParameter(param, value); });
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
    colorGroup_->setEnabled(enabled);
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
        curves_->setCurves(model::ToneCurves{});
        const model::ColorCorrection neutral;
        temperature_->setValue(neutral.temperature);
        tint_->setValue(neutral.tint);
        exposure_->setValue(neutral.exposure);
        contrast_->setValue(neutral.contrast);
        saturation_->setValue(neutral.saturation);
        gain_->setValue(0.0);
        pan_->setValue(0.0);
        enabled_->setChecked(false);
        updating_ = false;
        applyKeyframeButtons();
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
    colorGroup_->setVisible(isVideo);
    audioGroup_->setVisible(!isVideo);
    if (track != nullptr && track->isLocked()) {
        setEditingEnabled(false);
        title_->setText(title_->text() + "  (track locked)");
    }

    // Guarded, so writing the model's values into the widgets does not read as
    // the user changing them and bounce straight back.
    updating_ = true;
    // The values *at the playhead*, not the static ones: an animated parameter
    // has a different value at every frame, and showing the static one would
    // disagree with the picture on screen.
    const model::Transform transform = clip->transformAt(position_);
    positionX_->setValue(transform.positionX);
    positionY_->setValue(transform.positionY);
    scaleX_->setValue(transform.scaleX);
    scaleY_->setValue(transform.scaleY);
    rotation_->setValue(transform.rotationDegrees);
    anchorX_->setValue(transform.anchorX);
    anchorY_->setValue(transform.anchorY);
    opacity_->setValue(transform.opacity);
    blend_->setCurrentIndex(blend_->findData(static_cast<int>(clip->blend)));
    curves_->setCurves(clip->curves);
    const model::ColorCorrection color = clip->colorAt(position_);
    temperature_->setValue(color.temperature);
    tint_->setValue(color.tint);
    exposure_->setValue(color.exposure);
    contrast_->setValue(color.contrast);
    saturation_->setValue(color.saturation);
    gain_->setValue(clip->gainDbAt(position_));
    pan_->setValue(clip->panAt(position_));
    enabled_->setChecked(clip->enabled);
    updating_ = false;
    applyKeyframeButtons();
}

void EffectControls::addRow(QFormLayout* form, const QString& label, model::Param param,
                            QDoubleSpinBox* spin) {
    // A diamond for the stopwatch and a diamond for the keyframe: the same
    // symbol every editor uses, and the difference between them is that one
    // says "this parameter animates" and the other says "it has a value here".
    auto* stopwatch = new QToolButton(this);
    stopwatch->setText(QString::fromUtf8("\u23F1"));
    stopwatch->setCheckable(true);
    stopwatch->setToolTip("Animate this parameter");
    stopwatch->setAutoRaise(true);
    // Named so the widget can be found by what it controls rather than by its
    // position in the layout, which is what a self-test driving the real panel
    // needs and what stylesheets want anyway.
    stopwatch->setObjectName(QString{"stopwatch:"} + model::toString(param));

    auto* keyframe = new QToolButton(this);
    keyframe->setText(QString::fromUtf8("\u25C6"));
    keyframe->setCheckable(true);
    keyframe->setToolTip("Keyframe at the playhead");
    keyframe->setAutoRaise(true);
    keyframe->setObjectName(QString{"keyframe:"} + model::toString(param));

    // Square and compact, so two buttons per row do not push the value out of
    // the panel.
    const int side = stopwatch->fontMetrics().height() + 8;
    for (QToolButton* button : {stopwatch, keyframe}) {
        button->setFixedSize(side, side);
    }

    auto* line = new QWidget(this);
    auto* row = new QHBoxLayout(line);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);
    row->addWidget(stopwatch);
    row->addWidget(keyframe);
    row->addWidget(spin, 1);

    // The label keeps the width it needs. A form column is free to shrink to
    // whatever is left over, and "Position X" reading as "Position" next to
    // another row reading "Position" is worse than a narrow value field --
    // twice now a control has shipped with its name cut in half.
    auto* text = new QLabel(label, this);
    text->setMinimumWidth(text->sizeHint().width());
    form->addRow(text, line);

    connect(stopwatch, &QToolButton::clicked, this,
            [this, param](bool on) { toggleAnimated(param, on); });
    connect(keyframe, &QToolButton::clicked, this, [this, param] { toggleKeyframe(param); });

    rows_.push_back(Row{param, spin, stopwatch, keyframe});
}

void EffectControls::setPosition(const time::RationalTime& position) {
    position_ = position;
    // An animated parameter shows a different value at every frame, so moving
    // the playhead changes what the panel should say.
    applyToWidgets();
}

std::optional<time::RationalTime> EffectControls::keyframeTime() const {
    const model::Clip* clip = selectedClip();
    if (clip == nullptr || position_.rate().isZero()) {
        return std::nullopt;
    }
    if (!clip->timelineRange.contains(position_.rescaledTo(clip->start().rate()))) {
        return std::nullopt;
    }
    return clip->sourceTimeAt(position_);
}

void EffectControls::toggleAnimated(model::Param param, bool on) {
    const auto when = keyframeTime();
    if (updating_ || commands_ == nullptr || !clip_.isValid() || !when.has_value()) {
        applyToWidgets();  // put the button back where the model says it is
        return;
    }
    auto built = edit::makeSetParameterAnimated(*project_, {sequenceId_, track_}, clip_, param, on,
                                                position_);
    if (!built) {
        applyToWidgets();
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    applyToWidgets();
    emit keyframesChanged();
    emit edited();
}

void EffectControls::toggleKeyframe(model::Param param) {
    const auto when = keyframeTime();
    if (updating_ || commands_ == nullptr || !clip_.isValid() || !when.has_value()) {
        applyToWidgets();
        return;
    }
    const model::Clip* clip = selectedClip();
    const model::Curve* curve = clip != nullptr ? clip->animation.find(param) : nullptr;
    const bool exists = curve != nullptr && curve->at(*when) != nullptr;

    auto built =
        exists ? edit::makeRemoveKeyframe(*project_, {sequenceId_, track_}, clip_, param, *when)
               : edit::makeSetKeyframe(*project_, {sequenceId_, track_}, clip_, param, *when,
                                       clip->parameterAt(param, position_));
    if (!built) {
        applyToWidgets();
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    applyToWidgets();
    emit keyframesChanged();
    emit edited();
}

void EffectControls::pushParameter(model::Param param, double value) {
    if (updating_ || commands_ == nullptr || !clip_.isValid()) {
        return;
    }
    const model::Clip* clip = selectedClip();
    if (clip == nullptr) {
        return;
    }
    const model::Curve* curve = clip->animation.find(param);
    if (curve == nullptr || curve->empty()) {
        // Not animated: the old path, writing the static value.
        switch (param) {
            case model::Param::GainDb:
            case model::Param::Pan:
                pushAudio();
                return;
            case model::Param::Temperature:
            case model::Param::Tint:
            case model::Param::Exposure:
            case model::Param::Contrast:
            case model::Param::Saturation:
                pushColor();
                return;
            default:
                pushTransform();
                return;
        }
        return;
    }

    // Animated, so a value typed at the playhead is a keyframe there. Editing
    // the static value instead would appear to do nothing, since the curve wins
    // everywhere.
    const auto when = keyframeTime();
    if (!when.has_value()) {
        return;  // the playhead is not over this clip; there is nowhere to put it
    }
    auto built =
        edit::makeSetKeyframe(*project_, {sequenceId_, track_}, clip_, param, *when, value);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    applyKeyframeButtons();
    emit keyframesChanged();
    emit edited();
}

void EffectControls::applyKeyframeButtons() {
    const model::Clip* clip = selectedClip();
    const auto when = keyframeTime();
    for (const Row& row : rows_) {
        const model::Curve* curve = clip != nullptr ? clip->animation.find(row.param) : nullptr;
        const bool animated = curve != nullptr && !curve->empty();
        QSignalBlocker blockStopwatch{row.stopwatch};
        QSignalBlocker blockKeyframe{row.keyframe};
        row.stopwatch->setChecked(animated);
        // Only meaningful once the parameter animates, and only where the
        // playhead is actually over the clip.
        row.keyframe->setEnabled(animated && when.has_value());
        row.keyframe->setChecked(animated && when.has_value() && curve->at(*when) != nullptr);
        row.stopwatch->setEnabled(when.has_value());
    }
}

void EffectControls::pushTransform() {
    if (updating_ || commands_ == nullptr || !clip_.isValid()) {
        return;
    }
    const model::Clip* clip = selectedClip();
    if (clip == nullptr) {
        return;
    }
    // Start from what the clip already says, so a parameter that is animated
    // keeps the static value it had. The widget is showing that parameter's
    // *animated* value, and baking it into the static field would silently
    // change what the picture reverts to when animation is switched off.
    model::Transform transform = clip->transform;
    const auto isAnimated = [clip](model::Param param) {
        const model::Curve* curve = clip->animation.find(param);
        return curve != nullptr && !curve->empty();
    };
    if (!isAnimated(model::Param::PositionX)) {
        transform.positionX = positionX_->value();
    }
    if (!isAnimated(model::Param::PositionY)) {
        transform.positionY = positionY_->value();
    }
    if (!isAnimated(model::Param::ScaleX)) {
        transform.scaleX = scaleX_->value();
    }
    if (!isAnimated(model::Param::ScaleY)) {
        transform.scaleY = scaleY_->value();
    }
    if (!isAnimated(model::Param::RotationDegrees)) {
        transform.rotationDegrees = rotation_->value();
    }
    if (!isAnimated(model::Param::AnchorX)) {
        transform.anchorX = anchorX_->value();
    }
    if (!isAnimated(model::Param::AnchorY)) {
        transform.anchorY = anchorY_->value();
    }
    if (!isAnimated(model::Param::Opacity)) {
        transform.opacity = opacity_->value();
    }

    auto built = edit::makeSetTransform(*project_, {sequenceId_, track_}, clip_, transform);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    emit edited();
}

void EffectControls::pushCurves(const model::ToneCurves& curves, bool committed) {
    if (updating_ || commands_ == nullptr || !clip_.isValid()) {
        return;
    }
    auto built = edit::makeSetToneCurves(*project_, {sequenceId_, track_}, clip_, curves);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    if (committed) {
        // A drag is one undo step; the next gesture is a new one.
        commands_->breakMerge();
    }
    emit edited();
}

void EffectControls::pushColor() {
    if (updating_ || commands_ == nullptr || !clip_.isValid()) {
        return;
    }
    const model::Clip* clip = selectedClip();
    if (clip == nullptr) {
        return;
    }
    // The same rule as the transform: an animated parameter keeps its static
    // value, because the widget is showing the animated one.
    const auto staticOr = [clip](model::Param param, QDoubleSpinBox* spin) {
        const model::Curve* curve = clip->animation.find(param);
        return curve != nullptr && !curve->empty() ? clip->parameterValue(param) : spin->value();
    };
    model::ColorCorrection color;
    color.temperature = staticOr(model::Param::Temperature, temperature_);
    color.tint = staticOr(model::Param::Tint, tint_);
    color.exposure = staticOr(model::Param::Exposure, exposure_);
    color.contrast = staticOr(model::Param::Contrast, contrast_);
    color.saturation = staticOr(model::Param::Saturation, saturation_);

    auto built = edit::makeSetColorCorrection(*project_, {sequenceId_, track_}, clip_, color);
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
    const model::Clip* clip = selectedClip();
    if (clip == nullptr) {
        return;
    }
    const auto staticOr = [clip](model::Param param, QDoubleSpinBox* spin) {
        const model::Curve* curve = clip->animation.find(param);
        return curve != nullptr && !curve->empty() ? clip->parameterValue(param) : spin->value();
    };
    auto built = edit::makeSetClipAudio(*project_, {sequenceId_, track_}, clip_,
                                        staticOr(model::Param::GainDb, gain_),
                                        staticOr(model::Param::Pan, pan_));
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    emit edited();
}

}  // namespace zaro::app
