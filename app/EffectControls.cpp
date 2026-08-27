#include "EffectControls.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <fstream>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/model/ColorCorrection.h"
#include "zaro/core/render/BakeLut.h"
#include "zaro/core/render/Ducking.h"
#include "zaro/core/render/PathRaster.h"

#include "Say.h"

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

    // Pinning sits with Motion for the same reason stabilisation does: what it
    // changes is where the picture ends up.
    pin_ = new QPushButton("Pin to clip below", this);
    pin_->setObjectName("pin-to-clip");
    pin_->setToolTip("Follow the position and scale of whatever is under this at the playhead");
    unpin_ = new QPushButton("Unpin", this);
    unpin_->setObjectName("unpin");
    auto* pinRow = new QHBoxLayout;
    pinRow->setContentsMargins(0, 0, 0, 0);
    pinRow->addWidget(pin_);
    pinRow->addWidget(unpin_);
    motionForm->addRow(pinRow);
    connect(pin_, &QPushButton::clicked, this, [this] { emit pinRequested(); });
    connect(unpin_, &QPushButton::clicked, this, [this] { emit unpinRequested(); });

    // Stabilisation sits with Motion because that is what it changes, and next
    // to its own undo because an analysis somebody cannot throw away is one
    // they will not risk running.
    reframe_ = new QPushButton("Auto-reframe", this);
    reframe_->setObjectName("auto-reframe");
    reframe_->setToolTip("Fill the sequence's frame with this shot, following what is in it");
    motionForm->addRow(reframe_);
    connect(reframe_, &QPushButton::clicked, this, [this] { emit reframeRequested(); });

    stabilise_ = new QPushButton("Stabilise", this);
    stabilise_->setObjectName("stabilise");
    stabilise_->setToolTip("Analyse this clip's own frames and hold the picture still");
    unstabilise_ = new QPushButton("Clear", this);
    unstabilise_->setObjectName("stabilise-clear");
    unstabilise_->setToolTip("Throw the stabilisation away and put the framing back");
    auto* stabiliseRow = new QHBoxLayout;
    stabiliseRow->setContentsMargins(0, 0, 0, 0);
    stabiliseRow->addWidget(stabilise_);
    stabiliseRow->addWidget(unstabilise_);
    motionForm->addRow(stabiliseRow);
    connect(stabilise_, &QPushButton::clicked, this, [this] { emit stabiliseRequested(); });
    connect(unstabilise_, &QPushButton::clicked, this,
            [this] { emit clearStabilisationRequested(); });

    // Time remapping is a switch, not a stopwatch. Every other parameter has a
    // value the clip holds when nothing is animated; a remap that is not
    // animated is the clip's ordinary mapping, so there is nothing for a
    // stopwatch to turn off *to*.
    timeRemap_ = new QCheckBox("Time remap", this);
    timeRemap_->setObjectName("time-remap");
    timeRemap_->setToolTip("Pick which frame shows when, with keyframes");
    freeze_ = new QPushButton("Freeze here", this);
    freeze_->setObjectName("freeze-frame");
    freeze_->setToolTip("Hold the frame under the playhead for the whole clip");
    auto* remapRow = new QHBoxLayout;
    remapRow->setContentsMargins(0, 0, 0, 0);
    remapRow->addWidget(timeRemap_);
    remapRow->addWidget(freeze_);
    motionForm->addRow(remapRow);
    connect(timeRemap_, &QCheckBox::toggled, this, [this](bool on) {
        if (updating_ || commands_ == nullptr || !clip_.isValid()) {
            return;
        }
        auto built = edit::makeSetTimeRemapped(*project_, {sequenceId_, track_}, clip_, on);
        if (!built) {
            return;
        }
        commands_->execute(*project_, std::move(*built));
        commands_->breakMerge();
        applyToWidgets();
        emit edited();
    });
    connect(freeze_, &QPushButton::clicked, this, [this] {
        if (commands_ == nullptr || !clip_.isValid()) {
            return;
        }
        auto built = edit::makeFreezeFrame(*project_, {sequenceId_, track_}, clip_, position_);
        if (!built) {
            return;
        }
        commands_->execute(*project_, std::move(*built));
        commands_->breakMerge();
        applyToWidgets();
        emit edited();
    });
    videoGroup_ = motion;

    auto* colour = new QGroupBox("Colour", this);
    colourGroup_ = colour;
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

    // A look LUT. Shown inside the colour group, between the primary and the
    // curves, which is where it is applied.
    lutName_ = new QLabel("No LUT", this);
    lutName_->setObjectName("lut-name");
    lutLoad_ = new QPushButton("Load LUT...", this);
    lutClear_ = new QPushButton("Clear", this);
    lutSave_ = new QPushButton("Save look…", this);
    lutSave_->setObjectName("save-look");
    lutSave_->setToolTip("Write this clip's grade out as a .cube");
    lutAmount_ = makeSpin(0.0, 1.0, 0.05, 3);
    lutAmount_->setObjectName("lut-amount");

    auto* lutRow = new QWidget(this);
    auto* lutLayout = new QHBoxLayout(lutRow);
    lutLayout->setContentsMargins(0, 0, 0, 0);
    lutLayout->addWidget(lutLoad_);
    lutLayout->addWidget(lutClear_);
    lutLayout->addWidget(lutSave_);
    colourForm->addRow(lutRow);
    colourForm->addRow(lutName_);
    colourForm->addRow("LUT amount", lutAmount_);

    connect(lutLoad_, &QPushButton::clicked, this, [this] {
        const QString chosen = QFileDialog::getOpenFileName(this, "Open a .cube LUT", {},
                                                            "Cube LUTs (*.cube);;All files (*)");
        if (chosen.isEmpty()) {
            return;
        }
        model::LutRef lut;
        lut.path = chosen.toStdString();
        lut.amount = lutAmount_->value();
        pushLut(lut);
    });
    connect(lutClear_, &QPushButton::clicked, this, [this] { pushLut(model::LutRef{}); });
    connect(lutSave_, &QPushButton::clicked, this, [this] { saveLookAsCube(); });
    connect(lutAmount_, &QDoubleSpinBox::valueChanged, this, [this](double amount) {
        const model::Clip* clip = selectedClip();
        if (updating_ || clip == nullptr || clip->lut.path.empty()) {
            return;
        }
        model::LutRef lut = clip->lut;
        lut.amount = amount;
        pushLut(lut);
    });

    // The secondary. Its correction is deliberately a subset of the primary's:
    // temperature, exposure and saturation are what a keyed correction is
    // almost always for, and every extra control here is one more thing
    // between someone and the qualifier they are actually trying to set.
    qualifierOn_ = new QCheckBox("Key a colour range", this);
    qualifierOn_->setObjectName("qualifier-enabled");
    showMask_ = new QCheckBox("Show mask", this);
    showMask_->setObjectName("qualifier-show-mask");
    hueBand_ = new HueBand(this);
    hueCentre_ = makeSpin(0.0, 360.0, 5.0, 1, QString::fromUtf8("\u00B0"));
    hueWidth_ = makeSpin(0.0, 360.0, 5.0, 1, QString::fromUtf8("\u00B0"));
    hueSoftness_ = makeSpin(0.0, 180.0, 5.0, 1, QString::fromUtf8("\u00B0"));
    satLow_ = makeSpin(0.0, 1.0, 0.05, 3);
    satHigh_ = makeSpin(0.0, 1.0, 0.05, 3);
    lumaLow_ = makeSpin(0.0, 1.0, 0.05, 3);
    lumaHigh_ = makeSpin(0.0, 1.0, 0.05, 3);
    lumaHigh_->setObjectName("qualifier-luma-high");
    keyTemperature_ = makeSpin(-100.0, 100.0, 1.0, 1);
    keyExposure_ = makeSpin(-6.0, 6.0, 0.1, 2, " EV");
    keySaturation_ = makeSpin(0.0, 200.0, 1.0, 1);

    auto* secondary = new QGroupBox("Secondary", this);
    auto* secondaryForm = new QFormLayout(secondary);
    secondaryForm->addRow(qualifierOn_);
    secondaryForm->addRow(showMask_);
    secondaryForm->addRow(hueBand_);
    secondaryForm->addRow("Hue", hueCentre_);
    secondaryForm->addRow("Hue width", hueWidth_);
    secondaryForm->addRow("Hue softness", hueSoftness_);
    secondaryForm->addRow("Sat from", satLow_);
    secondaryForm->addRow("Sat to", satHigh_);
    secondaryForm->addRow("Luma from", lumaLow_);
    secondaryForm->addRow("Luma to", lumaHigh_);
    secondaryForm->addRow("Temperature", keyTemperature_);
    secondaryForm->addRow("Exposure", keyExposure_);
    secondaryForm->addRow("Saturation", keySaturation_);
    secondaryGroup_ = secondary;

    for (QDoubleSpinBox* spin : {hueCentre_, hueWidth_, hueSoftness_, satLow_, satHigh_, lumaLow_,
                                 lumaHigh_, keyTemperature_, keyExposure_, keySaturation_}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { pushSecondary(); });
    }
    for (QCheckBox* box : {qualifierOn_, showMask_}) {
        connect(box, &QCheckBox::toggled, this, [this] { pushSecondary(); });
    }
    connect(hueBand_, &HueBand::centreChanged, this, [this](double centre) {
        if (updating_) {
            return;
        }
        hueCentre_->setValue(centre);  // which pushes on its own
    });

    // A mask: where on the screen this clip shows through.
    maskShape_ = new QComboBox(this);
    maskShape_->setObjectName("mask-shape");
    maskShape_->addItem("none", static_cast<int>(model::MaskShape::None));
    maskShape_->addItem("rectangle", static_cast<int>(model::MaskShape::Rectangle));
    maskShape_->addItem("ellipse", static_cast<int>(model::MaskShape::Ellipse));
    maskWidth_ = makeSpin(0.0, 20000.0, 10.0, 1, " px");
    maskHeight_ = makeSpin(0.0, 20000.0, 10.0, 1, " px");
    maskX_ = makeSpin(-20000.0, 20000.0, 10.0, 1, " px");
    maskY_ = makeSpin(-20000.0, 20000.0, 10.0, 1, " px");
    maskCorner_ = makeSpin(0.0, 5000.0, 5.0, 1, " px");
    maskFeather_ = makeSpin(0.0, 2000.0, 2.0, 1, " px");
    maskInverted_ = new QCheckBox("Inverted", this);
    maskInverted_->setObjectName("mask-inverted");

    auto* maskBox = new QGroupBox("Mask", this);
    auto* maskForm = new QFormLayout(maskBox);
    maskForm->addRow("Shape", maskShape_);
    maskForm->addRow("Width", maskWidth_);
    maskForm->addRow("Height", maskHeight_);
    maskForm->addRow("Centre X", maskX_);
    maskForm->addRow("Centre Y", maskY_);
    maskForm->addRow("Corner", maskCorner_);
    maskForm->addRow("Feather", maskFeather_);
    maskForm->addRow(maskInverted_);
    maskToPath_ = new QPushButton("Convert to path", this);
    maskToPath_->setObjectName("mask-to-path");
    maskToPath_->setToolTip("Turn this shape into points you can drag on the picture");
    maskForm->addRow(maskToPath_);
    connect(maskToPath_, &QPushButton::clicked, this, [this] { convertMaskToPath(); });
    maskDraw_ = new QPushButton("Draw path", this);
    maskDraw_->setObjectName("mask-draw");
    maskDraw_->setCheckable(true);
    maskDraw_->setToolTip(
        "Click on the picture to lay down points; click the first one again to close");
    maskForm->addRow(maskDraw_);
    connect(maskDraw_, &QPushButton::toggled, this,
            [this](bool on) { emit drawMaskRequested(on); });
    maskTrack_ = new QPushButton("Track mask", this);
    maskTrack_->setObjectName("mask-track");
    maskTrack_->setToolTip("Follow what the mask is on through the rest of the clip");
    maskForm->addRow(maskTrack_);
    connect(maskTrack_, &QPushButton::clicked, this, [this] { emit trackMaskRequested(); });
    maskGroup_ = maskBox;

    for (QDoubleSpinBox* spin :
         {maskWidth_, maskHeight_, maskX_, maskY_, maskCorner_, maskFeather_}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { pushMask(); });
    }
    connect(maskShape_, &QComboBox::currentIndexChanged, this, [this] { pushMask(); });
    connect(maskInverted_, &QCheckBox::toggled, this, [this] { pushMask(); });

    // The effect stack. A list, because order is what a list has: blurring and
    // then sharpening is not the same picture as sharpening and then blurring.
    effectList_ = new QListWidget(this);
    effectList_->setObjectName("effect-list");
    effectList_->setMaximumHeight(90);
    effectKind_ = new QComboBox(this);
    effectKind_->setObjectName("effect-kind");
    for (const model::EffectKind kind : model::allEffectKinds()) {
        effectKind_->addItem(QString::fromUtf8(model::toString(kind)), static_cast<int>(kind));
    }
    effectAdd_ = new QPushButton("Add", this);
    effectAdd_->setObjectName("effect-add");
    effectRemove_ = new QPushButton("Remove", this);
    effectUp_ = new QPushButton("Up", this);
    effectDown_ = new QPushButton("Down", this);
    effectEnabled_ = new QCheckBox("Enabled", this);
    effectEnabled_->setObjectName("effect-enabled");

    auto* effectBox = new QGroupBox("Effects", this);
    auto* effectForm = new QFormLayout(effectBox);
    effectForm->addRow(effectList_);
    auto* addRowWidget = new QWidget(this);
    auto* addLayout = new QHBoxLayout(addRowWidget);
    addLayout->setContentsMargins(0, 0, 0, 0);
    addLayout->addWidget(effectKind_, 1);
    addLayout->addWidget(effectAdd_);
    effectForm->addRow(addRowWidget);
    auto* orderRow = new QWidget(this);
    auto* orderLayout = new QHBoxLayout(orderRow);
    orderLayout->setContentsMargins(0, 0, 0, 0);
    orderLayout->addWidget(effectUp_);
    orderLayout->addWidget(effectDown_);
    orderLayout->addWidget(effectRemove_);
    effectForm->addRow(orderRow);
    effectForm->addRow(effectEnabled_);
    for (int i = 0; i < kMaxEffectParams; ++i) {
        const auto slot = static_cast<std::size_t>(i);
        effectParamLabels_[slot] = new QLabel(this);
        effectParamSpins_[slot] = makeSpin(0.0, 1.0, 0.1, 3);
        effectParamSpins_[slot]->setObjectName(QString("effect-param-%1").arg(i));

        // The same two diamonds every other animatable row has, so an effect
        // parameter animates the way a position does rather than being a
        // parameter people have to learn is different.
        auto* stopwatch = new QToolButton(this);
        stopwatch->setText(QString::fromUtf8("\u23F1"));
        stopwatch->setCheckable(true);
        stopwatch->setAutoRaise(true);
        stopwatch->setToolTip("Animate this parameter");
        stopwatch->setObjectName(QString("effect-stopwatch-%1").arg(i));
        auto* keyframe = new QToolButton(this);
        keyframe->setText(QString::fromUtf8("\u25C6"));
        keyframe->setCheckable(true);
        keyframe->setAutoRaise(true);
        keyframe->setToolTip("Keyframe at the playhead");
        keyframe->setObjectName(QString("effect-keyframe-%1").arg(i));
        const int side = stopwatch->fontMetrics().height() + 8;
        for (QToolButton* button : {stopwatch, keyframe}) {
            button->setFixedSize(side, side);
        }
        effectParamStopwatches_[slot] = stopwatch;
        effectParamKeyframes_[slot] = keyframe;

        auto* line = new QWidget(this);
        auto* row = new QHBoxLayout(line);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);
        row->addWidget(stopwatch);
        row->addWidget(keyframe);
        row->addWidget(effectParamSpins_[slot], 1);
        effectForm->addRow(effectParamLabels_[slot], line);

        connect(effectParamSpins_[slot], &QDoubleSpinBox::valueChanged, this,
                [this] { pushEffects(); });
        connect(stopwatch, &QToolButton::clicked, this,
                [this, i](bool on) { toggleEffectAnimated(i, on); });
        connect(keyframe, &QToolButton::clicked, this, [this, i] { toggleEffectKeyframe(i); });
    }
    effectGroup_ = effectBox;

    connect(effectAdd_, &QPushButton::clicked, this, [this] {
        const model::Clip* clip = selectedClip();
        if (clip == nullptr) {
            return;
        }
        std::vector<model::Effect> stack = clip->effects;
        model::Effect added;
        added.kind = static_cast<model::EffectKind>(effectKind_->currentData().toInt());
        // Appended rather than inserted at the selection: an effect goes on top
        // of what is there, which is what "add" means everywhere else.
        stack.push_back(added);
        const auto last = static_cast<int>(stack.size()) - 1;
        if (applyEffectStack(stack)) {
            {
                const QSignalBlocker quiet{effectList_};
                effectList_->addItem(QString::fromUtf8(model::toString(added.kind)));
                effectList_->setCurrentRow(last);
            }
            showEffects();
        }
    });
    connect(effectRemove_, &QPushButton::clicked, this, [this] {
        const model::Clip* clip = selectedClip();
        const int row = effectList_->currentRow();
        if (clip == nullptr || row < 0 || row >= static_cast<int>(clip->effects.size())) {
            return;
        }
        std::vector<model::Effect> stack = clip->effects;
        stack.erase(stack.begin() + row);
        if (applyEffectStack(stack)) {
            effectList_->setCurrentRow(std::min(row, static_cast<int>(stack.size()) - 1));
            showEffects();
        }
    });
    const auto move = [this](int delta) {
        const model::Clip* clip = selectedClip();
        const int row = effectList_->currentRow();
        const int to = row + delta;
        if (clip == nullptr || row < 0 || to < 0 || to >= static_cast<int>(clip->effects.size())) {
            return;
        }
        std::vector<model::Effect> stack = clip->effects;
        std::swap(stack[static_cast<std::size_t>(row)], stack[static_cast<std::size_t>(to)]);
        if (applyEffectStack(stack)) {
            effectList_->setCurrentRow(to);
            showEffects();
        }
    };
    connect(effectUp_, &QPushButton::clicked, this, [move] { move(-1); });
    connect(effectDown_, &QPushButton::clicked, this, [move] { move(1); });
    connect(effectList_, &QListWidget::currentRowChanged, this, [this] { showEffects(); });
    connect(effectEnabled_, &QCheckBox::toggled, this, [this] { pushEffects(); });

    // The three wheels. Nine numbers rather than three pucks: the arithmetic
    // is what makes a grade, and a puck is a way of typing two of these at
    // once. The engine is an ASC CDL either way -- see model::ColorWheels --
    // so a circular control can be put in front of exactly these values later
    // without anything behind them moving.
    auto* wheelsBox = new QGroupBox("Wheels", this);
    auto* wheelsForm = new QFormLayout(wheelsBox);
    static constexpr const char* kWheelRows[] = {"Shadows", "Midtones", "Highlights"};
    for (int row = 0; row < 3; ++row) {
        auto* line = new QWidget(this);
        auto* columns = new QHBoxLayout(line);
        columns->setContentsMargins(0, 0, 0, 0);
        for (int channel = 0; channel < 3; ++channel) {
            const auto slot = static_cast<std::size_t>((row * 3) + channel);
            // Offsets are signed and small; slopes and powers are multipliers
            // around one. Ranges that match what the number means, so dragging
            // one is a grade rather than a search.
            wheels_[slot] = row == 0 ? makeSpin(-1.0, 1.0, 0.01, 3) : makeSpin(0.0, 4.0, 0.01, 3);
            wheels_[slot]->setObjectName(QString("wheel-%1-%2").arg(row).arg(channel));
            columns->addWidget(wheels_[slot]);
            connect(wheels_[slot], &QDoubleSpinBox::valueChanged, this, [this] { pushWheels(); });
        }
        wheelsForm->addRow(QString::fromUtf8(kWheelRows[row]), line);
    }
    colourForm->addRow(wheelsBox);

    // The vignette. In the colour group because that is where somebody reaches
    // for it, even though the arithmetic is geometry -- see model::Vignette.
    vignetteAmount_ = makeSpin(-1.0, 1.0, 0.05, 2);
    vignetteAmount_->setObjectName("vignette-amount");
    vignetteMidpoint_ = makeSpin(0.0, 2.0, 0.05, 2);
    vignetteFeather_ = makeSpin(0.0, 2.0, 0.05, 2);
    vignetteRoundness_ = makeSpin(0.0, 1.0, 0.05, 2);
    colourForm->addRow("Vignette", vignetteAmount_);
    colourForm->addRow("  midpoint", vignetteMidpoint_);
    colourForm->addRow("  feather", vignetteFeather_);
    colourForm->addRow("  roundness", vignetteRoundness_);
    for (QDoubleSpinBox* spin :
         {vignetteAmount_, vignetteMidpoint_, vignetteFeather_, vignetteRoundness_}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { pushVignette(); });
    }

    // The keyer: what of this clip is transparent. Its own group rather than a
    // corner of the secondary, because it looks like a qualifier and answers a
    // different question -- one is "which pixels to correct", the other is
    // "which pixels are there at all".
    keyKind_ = new QComboBox(this);
    keyKind_->setObjectName("key-kind");
    keyKind_->addItem("none", static_cast<int>(model::KeyKind::None));
    keyKind_->addItem("colour", static_cast<int>(model::KeyKind::Chroma));
    keyKind_->addItem("brightness", static_cast<int>(model::KeyKind::Luma));
    keyRed_ = makeSpin(0.0, 1.0, 0.05, 3);
    keyGreen_ = makeSpin(0.0, 1.0, 0.05, 3);
    keyBlue_ = makeSpin(0.0, 1.0, 0.05, 3);
    keyTolerance_ = makeSpin(0.0, 1.0, 0.01, 3);
    keySoftness_ = makeSpin(0.0, 1.0, 0.01, 3);
    keySpill_ = makeSpin(0.0, 1.0, 0.05, 2);
    keySpill_->setObjectName("key-spill");
    keyLumaLow_ = makeSpin(0.0, 1.0, 0.01, 3);
    keyLumaHigh_ = makeSpin(0.0, 1.0, 0.01, 3);
    keyShowMatte_ = new QCheckBox("Show the matte", this);
    keyShowMatte_->setObjectName("key-matte");

    auto* keyBox = new QGroupBox("Key", this);
    auto* keyForm = new QFormLayout(keyBox);
    keyForm->addRow("Key", keyKind_);
    keyForm->addRow("Red", keyRed_);
    keyForm->addRow("Green", keyGreen_);
    keyForm->addRow("Blue", keyBlue_);
    keyForm->addRow("Tolerance", keyTolerance_);
    keyForm->addRow("Softness", keySoftness_);
    keyForm->addRow("Spill", keySpill_);
    keyForm->addRow("Dark from", keyLumaLow_);
    keyForm->addRow("Dark to", keyLumaHigh_);
    keyForm->addRow(keyShowMatte_);
    keyGroup_ = keyBox;

    for (QDoubleSpinBox* spin : {keyRed_, keyGreen_, keyBlue_, keyTolerance_, keySoftness_,
                                 keySpill_, keyLumaLow_, keyLumaHigh_}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { pushKeyer(); });
    }
    connect(keyKind_, &QComboBox::currentIndexChanged, this, [this] { pushKeyer(); });
    connect(keyShowMatte_, &QCheckBox::toggled, this, [this] { pushKeyer(); });

    // A generated shape. Shown only for a clip that is one: the controls are
    // meaningless on a clip with media, and a panel full of inert fields is
    // worse than one that says nothing.
    shapeKind_ = new QComboBox(this);
    shapeKind_->setObjectName("shape-kind");
    for (const model::GraphicKind kind :
         {model::GraphicKind::Rectangle, model::GraphicKind::Ellipse}) {
        shapeKind_->addItem(QString::fromUtf8(model::toString(kind)), static_cast<int>(kind));
    }
    shapeWidth_ = makeSpin(0.0, 20000.0, 10.0, 1, " px");
    shapeHeight_ = makeSpin(0.0, 20000.0, 10.0, 1, " px");
    shapeCorner_ = makeSpin(0.0, 5000.0, 5.0, 1, " px");
    shapeFeather_ = makeSpin(0.0, 2000.0, 2.0, 1, " px");
    shapeRed_ = makeSpin(0.0, 1.0, 0.05, 3);
    shapeGreen_ = makeSpin(0.0, 1.0, 0.05, 3);
    shapeBlue_ = makeSpin(0.0, 1.0, 0.05, 3);

    auto* graphic = new QGroupBox("Shape", this);
    auto* graphicForm = new QFormLayout(graphic);
    graphicForm->addRow("Kind", shapeKind_);
    graphicForm->addRow("Width", shapeWidth_);
    graphicForm->addRow("Height", shapeHeight_);
    graphicForm->addRow("Corner", shapeCorner_);
    graphicForm->addRow("Feather", shapeFeather_);
    graphicForm->addRow("Red", shapeRed_);
    graphicForm->addRow("Green", shapeGreen_);
    graphicForm->addRow("Blue", shapeBlue_);

    // Responsive timing lives with the shape because it is a title's problem:
    // a graphic is the thing whose animation has a beginning and an end that
    // are supposed to stay put while the middle takes up the slack.
    introSeconds_ = makeSpin(0.0, 60.0, 0.1, 2, " s");
    introSeconds_->setObjectName("responsive-intro");
    outroSeconds_ = makeSpin(0.0, 60.0, 0.1, 2, " s");
    outroSeconds_->setObjectName("responsive-outro");
    introSeconds_->setToolTip("Keep this much of the animation glued to the start of the clip");
    outroSeconds_->setToolTip("Keep this much of the animation glued to the end of the clip");
    graphicForm->addRow("Intro", introSeconds_);
    graphicForm->addRow("Outro", outroSeconds_);
    for (QDoubleSpinBox* spin : {introSeconds_, outroSeconds_}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { pushResponsive(); });
    }
    graphicGroup_ = graphic;

    for (QDoubleSpinBox* spin : {shapeWidth_, shapeHeight_, shapeCorner_, shapeFeather_, shapeRed_,
                                 shapeGreen_, shapeBlue_}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { pushGraphic(); });
    }
    connect(shapeKind_, &QComboBox::currentIndexChanged, this, [this] { pushGraphic(); });

    auto* audio = new QGroupBox("Audio", this);
    auto* audioForm = new QFormLayout(audio);
    addRow(audioForm, "Gain", model::Param::GainDb, gain_);
    addRow(audioForm, "Pan", model::Param::Pan, pan_);

    // What the sound is for, and the one decision that reads it. Together,
    // because a role with nothing acting on it is a label somebody sets once
    // and never sees again.
    role_ = new QComboBox(this);
    role_->setObjectName("audio-role");
    for (const model::AudioRole kind : model::allAudioRoles()) {
        role_->addItem(QString::fromUtf8(model::toString(kind)), static_cast<int>(kind));
    }
    audioForm->addRow("Role", role_);
    duck_ = new QPushButton("Duck under dialogue", this);
    duck_->setObjectName("duck-under-dialogue");
    duck_->setToolTip("Write volume keyframes that keep this clip out of the way of speech");
    audioForm->addRow(duck_);
    connect(role_, &QComboBox::currentIndexChanged, this, [this] { pushRole(); });
    connect(duck_, &QPushButton::clicked, this, [this] { duckUnderDialogue(); });
    audioGroup_ = audio;

    // Scrolled, because the panel is now taller than a short display: motion,
    // colour, a curve editor, a secondary and audio. Without this the last
    // group is simply unreachable, with nothing on screen to suggest it exists.
    auto* inner = new QWidget;
    auto* layout = new QVBoxLayout(inner);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(title_);
    layout->addWidget(enabled_);
    layout->addWidget(motion);
    layout->addWidget(graphic);
    layout->addWidget(maskBox);
    layout->addWidget(colour);
    layout->addWidget(secondary);
    layout->addWidget(keyBox);
    layout->addWidget(effectBox);
    layout->addWidget(audio);
    layout->addStretch(1);

    auto* scroll = new QScrollArea(this);
    scroll_ = scroll;
    scroll->setWidget(inner);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    // Never horizontally: the controls already keep the width they need, and a
    // horizontal scrollbar would hide the values rather than reveal them.
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

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

void EffectControls::bind(const ui::SequenceBinding& binding) {
    project_ = binding.project;
    sequenceId_ = binding.sequence;
    commands_ = binding.commands;
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

/// Put a stage of the grade chain on screen.
///
/// Scrolled to rather than switched to: the panel is one column of groups in
/// render order, and a colourist reading it wants to see that the LUT sits
/// after the secondary. Hiding the rest to show one would take that away.
void EffectControls::revealStage(int stage) {
    if (scroll_ == nullptr) {
        return;
    }
    QWidget* target = nullptr;
    switch (stage) {
        case 0:
            target = colourGroup_;
            break;
        case 1:
            target = curves_;
            break;
        case 2:
            target = secondaryGroup_;
            break;
        case 3:
            target = lutName_;
            break;
        default:
            return;
    }
    if (target != nullptr) {
        scroll_->ensureWidgetVisible(target, 0, 40);
    }
}

void EffectControls::setEditingEnabled(bool enabled) {
    enabled_->setEnabled(enabled);
    videoGroup_->setEnabled(enabled);
    graphicGroup_->setEnabled(enabled);
    maskGroup_->setEnabled(enabled);
    colorGroup_->setEnabled(enabled);
    secondaryGroup_->setEnabled(enabled);
    keyGroup_->setEnabled(enabled);
    effectGroup_->setEnabled(enabled);
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
        timeRemap_->setChecked(false);
        role_->setCurrentIndex(role_->findData(static_cast<int>(model::AudioRole::Unassigned)));
        keyKind_->setCurrentIndex(keyKind_->findData(static_cast<int>(model::KeyKind::None)));
        keyShowMatte_->setChecked(false);
        effectList_->clear();
        showEffects();
        graphicGroup_->setVisible(false);
        maskGroup_->setVisible(false);
        curves_->setCurves(model::ToneCurves{});
        lutName_->setText("No LUT");
        lutAmount_->setValue(1.0);
        const model::Secondary blank;
        qualifierOn_->setChecked(false);
        showMask_->setChecked(false);
        hueCentre_->setValue(blank.qualifier.hueCentre);
        hueWidth_->setValue(blank.qualifier.hueWidth);
        hueSoftness_->setValue(blank.qualifier.hueSoftness);
        satLow_->setValue(blank.qualifier.saturationLow);
        satHigh_->setValue(blank.qualifier.saturationHigh);
        lumaLow_->setValue(blank.qualifier.lumaLow);
        lumaHigh_->setValue(blank.qualifier.lumaHigh);
        keyTemperature_->setValue(blank.correction.temperature);
        keyExposure_->setValue(blank.correction.exposure);
        keySaturation_->setValue(blank.correction.saturation);
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
    secondaryGroup_->setVisible(isVideo);
    keyGroup_->setVisible(isVideo);
    effectGroup_->setVisible(isVideo);
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
    maskGroup_->setVisible(isVideo);
    maskShape_->setCurrentIndex(maskShape_->findData(static_cast<int>(clip->mask.shape)));
    maskWidth_->setValue(clip->mask.width);
    maskHeight_->setValue(clip->mask.height);
    maskX_->setValue(clip->mask.centreX);
    maskY_->setValue(clip->mask.centreY);
    maskCorner_->setValue(clip->mask.cornerRadius);
    maskFeather_->setValue(clip->mask.feather);
    maskInverted_->setChecked(clip->mask.inverted);
    // Nothing to convert from without a shape, and nothing to convert *to*
    // once it is already a path.
    maskToPath_->setEnabled(clip->mask.isSet() && clip->mask.shape != model::MaskShape::Path);
    maskDraw_->setEnabled(true);
    maskTrack_->setEnabled(clip->mask.isSet());
    pin_->setEnabled(isVideo);
    unpin_->setEnabled(clip->pinnedTo.isValid());
    const bool stabilised = clip->animation.find(model::Param::StabiliseX) != nullptr;
    stabilise_->setEnabled(clip->graphic.kind == model::GraphicKind::None &&
                           !clip->nested.isValid());
    unstabilise_->setEnabled(stabilised);
    reframe_->setEnabled(isVideo);

    graphicGroup_->setVisible(isVideo && clip->graphic.isSet());
    if (clip->graphic.isSet()) {
        shapeKind_->setCurrentIndex(shapeKind_->findData(static_cast<int>(clip->graphic.kind)));
        shapeWidth_->setValue(clip->graphic.width);
        shapeHeight_->setValue(clip->graphic.height);
        shapeCorner_->setValue(clip->graphic.cornerRadius);
        shapeFeather_->setValue(clip->graphic.feather);
        shapeRed_->setValue(clip->graphic.red);
        shapeGreen_->setValue(clip->graphic.green);
        shapeBlue_->setValue(clip->graphic.blue);
        introSeconds_->setValue(clip->responsive.intro.toSecondsDouble());
        outroSeconds_->setValue(clip->responsive.outro.toSecondsDouble());
    }

    curves_->setCurves(clip->curves);
    // The file name, not the path: the path is usually longer than the panel
    // and its useful end is the last part anyway.
    const QString path = QString::fromStdString(clip->lut.path);
    lutName_->setText(path.isEmpty() ? "No LUT" : path.section('/', -1));
    lutName_->setToolTip(path);
    lutAmount_->setValue(clip->lut.amount);
    lutAmount_->setEnabled(!path.isEmpty());
    lutClear_->setEnabled(!path.isEmpty());

    showEffects();

    const model::Keyer& key = clip->keyer;
    keyKind_->setCurrentIndex(keyKind_->findData(static_cast<int>(key.kind)));
    keyRed_->setValue(key.red);
    keyGreen_->setValue(key.green);
    keyBlue_->setValue(key.blue);
    keyTolerance_->setValue(key.tolerance);
    keySoftness_->setValue(key.softness);
    keySpill_->setValue(key.spill);
    keyLumaLow_->setValue(key.lumaLow);
    keyLumaHigh_->setValue(key.lumaHigh);
    keyShowMatte_->setChecked(key.showMatte);
    // A colour key's controls are meaningless on a brightness key and the other
    // way round. Disabled rather than hidden: the panel keeps its shape, so
    // switching kinds does not make everything below jump.
    const bool chroma = key.kind == model::KeyKind::Chroma;
    const bool luma = key.kind == model::KeyKind::Luma;
    for (QDoubleSpinBox* spin :
         {keyRed_, keyGreen_, keyBlue_, keyTolerance_, keySoftness_, keySpill_}) {
        spin->setEnabled(chroma);
    }
    keyLumaLow_->setEnabled(luma);
    keyLumaHigh_->setEnabled(luma);
    keyShowMatte_->setEnabled(key.isSet());

    vignetteAmount_->setValue(clip->vignette.amount);
    vignetteMidpoint_->setValue(clip->vignette.midpoint);
    vignetteFeather_->setValue(clip->vignette.feather);
    vignetteRoundness_->setValue(clip->vignette.roundness);

    const model::ColorWheels& wheels = clip->wheels;
    wheels_[0]->setValue(wheels.offsetR);
    wheels_[1]->setValue(wheels.offsetG);
    wheels_[2]->setValue(wheels.offsetB);
    wheels_[3]->setValue(wheels.powerR);
    wheels_[4]->setValue(wheels.powerG);
    wheels_[5]->setValue(wheels.powerB);
    wheels_[6]->setValue(wheels.slopeR);
    wheels_[7]->setValue(wheels.slopeG);
    wheels_[8]->setValue(wheels.slopeB);

    role_->setCurrentIndex(role_->findData(static_cast<int>(clip->role)));
    // Ducking is for a bed that has to get out of the way; a clip that is
    // itself the dialogue has nothing to duck under.
    duck_->setEnabled(audio_ != nullptr && clip->role != model::AudioRole::Unassigned &&
                      clip->role != model::AudioRole::Dialogue);

    timeRemap_->setChecked(clip->isTimeRemapped());
    // Freezing a clip that is already frozen is a no-op the operation refuses,
    // so the button says so rather than letting somebody find out.
    freeze_->setEnabled(clip->timelineRange.contains(position_));

    const model::Secondary& keyed = clip->secondary;
    qualifierOn_->setChecked(keyed.qualifier.enabled);
    showMask_->setChecked(keyed.showMask);
    hueCentre_->setValue(keyed.qualifier.hueCentre);
    hueWidth_->setValue(keyed.qualifier.hueWidth);
    hueSoftness_->setValue(keyed.qualifier.hueSoftness);
    satLow_->setValue(keyed.qualifier.saturationLow);
    satHigh_->setValue(keyed.qualifier.saturationHigh);
    lumaLow_->setValue(keyed.qualifier.lumaLow);
    lumaHigh_->setValue(keyed.qualifier.lumaHigh);
    keyTemperature_->setValue(keyed.correction.temperature);
    keyExposure_->setValue(keyed.correction.exposure);
    keySaturation_->setValue(keyed.correction.saturation);
    hueBand_->setWindow(keyed.qualifier.hueCentre, keyed.qualifier.hueWidth,
                        keyed.qualifier.hueSoftness);
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
    return clip->baseSourceTimeAt(position_);
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

model::Secondary EffectControls::secondaryFromWidgets() const {
    model::Secondary out;
    out.qualifier.enabled = qualifierOn_->isChecked();
    out.showMask = showMask_->isChecked();
    out.qualifier.hueCentre = hueCentre_->value();
    out.qualifier.hueWidth = hueWidth_->value();
    out.qualifier.hueSoftness = hueSoftness_->value();
    out.qualifier.saturationLow = satLow_->value();
    out.qualifier.saturationHigh = satHigh_->value();
    out.qualifier.lumaLow = lumaLow_->value();
    out.qualifier.lumaHigh = lumaHigh_->value();
    // Softness for saturation and luma is not exposed: two more spin boxes for
    // a number nobody reaches for, when the default already keeps the edge from
    // stepping. It stays in the model, and in the file.
    const model::HslQualifier defaults;
    out.qualifier.saturationSoftness = defaults.saturationSoftness;
    out.qualifier.lumaSoftness = defaults.lumaSoftness;

    out.correction.temperature = keyTemperature_->value();
    out.correction.exposure = keyExposure_->value();
    out.correction.saturation = keySaturation_->value();
    return out;
}

/// Rebuild the list and the parameter rows from the selected clip.
///
/// Driven from `model::parametersOf` rather than from a hand-written set of
/// controls: the claim that adding an effect is data only holds if the panel
/// reads the same table the renderer does.
bool EffectControls::applyEffectStack(const std::vector<model::Effect>& stack) {
    if (commands_ == nullptr || !clip_.isValid()) {
        return false;
    }
    auto built = edit::makeSetEffects(*project_, {sequenceId_, track_}, clip_, stack);
    if (!built) {
        return false;
    }
    commands_->execute(*project_, std::move(*built));
    emit edited();
    return true;
}

void EffectControls::showEffects() {
    const model::Clip* clip = selectedClip();
    const bool wasUpdating = updating_;
    updating_ = true;

    const int wanted = effectList_->currentRow();
    {
        // Blocked while rebuilding: clearing a list emits currentRowChanged,
        // which is wired back to this function -- so without this it re-enters
        // itself halfway through, having already thrown away the selection it
        // is in the middle of restoring.
        const QSignalBlocker quiet{effectList_};
        effectList_->clear();
        if (clip != nullptr) {
            for (const model::Effect& effect : clip->effects) {
                QString label = QString::fromUtf8(model::toString(effect.kind));
                if (!effect.enabled) {
                    label += " (off)";
                }
                effectList_->addItem(label);
            }
            if (wanted >= 0 && wanted < static_cast<int>(clip->effects.size())) {
                effectList_->setCurrentRow(wanted);
            }
        }
    }

    const int row = effectList_->currentRow();
    const model::Effect* chosen =
        clip != nullptr && row >= 0 && row < static_cast<int>(clip->effects.size())
            ? &clip->effects[static_cast<std::size_t>(row)]
            : nullptr;
    effectEnabled_->setChecked(chosen != nullptr && chosen->enabled);
    effectEnabled_->setEnabled(chosen != nullptr);
    effectRemove_->setEnabled(chosen != nullptr);
    effectUp_->setEnabled(chosen != nullptr && row > 0);
    effectDown_->setEnabled(clip != nullptr && chosen != nullptr &&
                            row + 1 < static_cast<int>(clip->effects.size()));

    std::size_t shown = 0;
    if (chosen != nullptr) {
        for (const model::EffectParamInfo& info : model::parametersOf(chosen->kind)) {
            if (shown >= static_cast<std::size_t>(kMaxEffectParams)) {
                break;
            }
            QLabel* label = effectParamLabels_[shown];
            QDoubleSpinBox* spin = effectParamSpins_[shown];
            QString name = QString::fromUtf8(model::toString(info.param));
            name[0] = name[0].toUpper();
            label->setText(name);
            spin->setRange(info.minimum, info.maximum);
            spin->setSingleStep(info.step);
            effectParamOf_[shown] = info.param;

            // The value at the playhead, not the static one: an animated
            // parameter reads differently on every frame, and a control showing
            // the value it had before it was animated is a control that lies.
            const model::Clip* owner = selectedClip();
            const double seconds = owner != nullptr ? owner->sourceSecondsAt(position_) : 0.0;
            spin->setValue(chosen->valueAt(info.param, seconds));

            const bool animated = chosen->isAnimated(info.param);
            const auto when = keyframeTime();
            effectParamStopwatches_[shown]->setChecked(animated);
            effectParamStopwatches_[shown]->setEnabled(when.has_value());
            effectParamKeyframes_[shown]->setEnabled(animated && when.has_value());
            effectParamKeyframes_[shown]->setChecked(
                animated && when.has_value() && chosen->curve(info.param)->at(*when) != nullptr);

            label->setVisible(true);
            spin->setVisible(true);
            effectParamStopwatches_[shown]->setVisible(true);
            effectParamKeyframes_[shown]->setVisible(true);
            ++shown;
        }
    }
    for (std::size_t i = shown; i < static_cast<std::size_t>(kMaxEffectParams); ++i) {
        effectParamLabels_[i]->setVisible(false);
        effectParamSpins_[i]->setVisible(false);
        effectParamStopwatches_[i]->setVisible(false);
        effectParamKeyframes_[i]->setVisible(false);
    }

    updating_ = wasUpdating;
}

/// Turn a curve on or off for one effect parameter.
///
/// Switching the stopwatch on seeds a keyframe at the playhead holding the
/// value that is showing, so the picture does not move; switching it off keeps
/// the value that is showing as the static one, so it does not move then
/// either. The same bargain the clip's own parameters make.
void EffectControls::toggleEffectAnimated(int row, bool on) {
    const model::Clip* clip = selectedClip();
    const int chosen = effectList_->currentRow();
    const auto when = keyframeTime();
    if (updating_ || clip == nullptr || !when.has_value() || chosen < 0 ||
        chosen >= static_cast<int>(clip->effects.size())) {
        showEffects();  // put the button back where the model says it is
        return;
    }
    const model::EffectParam param = effectParamOf_[static_cast<std::size_t>(row)];

    std::vector<model::Effect> stack = clip->effects;
    model::Effect& effect = stack[static_cast<std::size_t>(chosen)];
    const double showing = effect.valueAt(param, clip->sourceSecondsAt(position_));
    if (on) {
        model::Keyframe key;
        key.time = *when;
        key.value = showing;
        effect.animation[param].set(key);
    } else {
        effect.animation.erase(param);
        effect.setValue(param, showing);
    }
    if (applyEffectStack(stack)) {
        commands_->breakMerge();
        showEffects();
        emit keyframesChanged();
    }
}

void EffectControls::toggleEffectKeyframe(int row) {
    const model::Clip* clip = selectedClip();
    const int chosen = effectList_->currentRow();
    const auto when = keyframeTime();
    if (updating_ || clip == nullptr || !when.has_value() || chosen < 0 ||
        chosen >= static_cast<int>(clip->effects.size())) {
        showEffects();
        return;
    }
    const model::EffectParam param = effectParamOf_[static_cast<std::size_t>(row)];
    if (!clip->effects[static_cast<std::size_t>(chosen)].isAnimated(param)) {
        // A keyframe on a parameter that is not animated has nowhere to go.
        // Rather than quietly turning animation on, the button is inert until
        // the stopwatch is -- which is what makes the two mean different things.
        showEffects();
        return;
    }

    std::vector<model::Effect> stack = clip->effects;
    model::Effect& effect = stack[static_cast<std::size_t>(chosen)];
    model::Curve& curve = effect.animation[param];
    if (curve.at(*when) != nullptr) {
        curve.removeAt(*when);
        if (curve.empty()) {
            // The last keyframe gone is no animation, and a curve with nothing
            // in it must not linger as a parameter that reads zero everywhere.
            effect.animation.erase(param);
        }
    } else {
        model::Keyframe key;
        key.time = *when;
        key.value = effect.valueAt(param, clip->sourceSecondsAt(position_));
        curve.set(key);
    }
    if (applyEffectStack(stack)) {
        commands_->breakMerge();
        showEffects();
        emit keyframesChanged();
    }
}

void EffectControls::pushEffects() {
    const model::Clip* clip = selectedClip();
    const int row = effectList_->currentRow();
    if (updating_ || clip == nullptr || row < 0 || row >= static_cast<int>(clip->effects.size())) {
        return;
    }
    std::vector<model::Effect> stack = clip->effects;
    model::Effect& chosen = stack[static_cast<std::size_t>(row)];
    chosen.enabled = effectEnabled_->isChecked();

    const auto when = keyframeTime();
    std::size_t index = 0;
    bool keyed = false;
    for (const model::EffectParamInfo& info : model::parametersOf(chosen.kind)) {
        if (index >= static_cast<std::size_t>(kMaxEffectParams)) {
            break;
        }
        const double typed = effectParamSpins_[index]->value();
        if (chosen.isAnimated(info.param)) {
            // Editing an animated parameter writes a keyframe at the playhead
            // rather than a static value nothing would read. Anything else
            // means turning a knob and watching the number spring back the
            // moment the playhead moves.
            if (when.has_value()) {
                model::Keyframe key;
                key.time = *when;
                key.value = typed;
                chosen.animation[info.param].set(key);
                keyed = true;
            }
        } else {
            chosen.setValue(info.param, typed);
        }
        ++index;
    }
    if (applyEffectStack(stack)) {
        showEffects();
        if (keyed) {
            emit keyframesChanged();
        }
    }
}

/// Write this clip's grade out as a .cube.
///
/// What could not be carried is said before the file is written, not after: a
/// look file that silently does not match the shot it came from is worse than
/// no look file, and the moment to find out is while somebody is still deciding
/// whether to write it.
void EffectControls::saveLookAsCube() {
    const model::Clip* clip = selectedClip();
    const model::Sequence* sequence =
        project_ != nullptr ? project_->findSequence(sequenceId_) : nullptr;
    if (clip == nullptr || sequence == nullptr) {
        return;
    }

    render::LutOmissions omissions;
    auto text = render::bakeCube(*clip, sequence->output().transfer, 33,
                                 clip->name.empty() ? "Look" : clip->name, &omissions);
    if (!text) {
        app::warn(this, "Save look", QString::fromStdString(text.error().toString()));
        return;
    }
    if (omissions.any()) {
        const auto answer = QMessageBox::question(
            this, "Save look",
            QString::fromStdString(omissions.describe()) + "\n\nWrite it anyway?");
        if (answer != QMessageBox::Yes) {
            return;
        }
    }

    const QString chosen =
        QFileDialog::getSaveFileName(this, "Save look", "look.cube", "Cube LUTs (*.cube)");
    if (chosen.isEmpty()) {
        return;
    }
    std::ofstream file{chosen.toStdString(), std::ios::binary | std::ios::trunc};
    if (!file) {
        app::warn(this, "Save look", "Could not open that file for writing.");
        return;
    }
    file << *text;
    if (!file) {
        app::warn(this, "Save look", "Failed while writing that file.");
    }
}

void EffectControls::pushVignette() {
    if (updating_ || commands_ == nullptr || !clip_.isValid()) {
        return;
    }
    model::Vignette vignette;
    vignette.amount = vignetteAmount_->value();
    vignette.midpoint = vignetteMidpoint_->value();
    vignette.feather = vignetteFeather_->value();
    vignette.roundness = vignetteRoundness_->value();

    auto built = edit::makeSetVignette(*project_, {sequenceId_, track_}, clip_, vignette);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    applyToWidgets();
    emit edited();
}

void EffectControls::pushWheels() {
    if (updating_ || commands_ == nullptr || !clip_.isValid()) {
        return;
    }
    model::ColorWheels wheels;
    wheels.offsetR = wheels_[0]->value();
    wheels.offsetG = wheels_[1]->value();
    wheels.offsetB = wheels_[2]->value();
    wheels.powerR = wheels_[3]->value();
    wheels.powerG = wheels_[4]->value();
    wheels.powerB = wheels_[5]->value();
    wheels.slopeR = wheels_[6]->value();
    wheels.slopeG = wheels_[7]->value();
    wheels.slopeB = wheels_[8]->value();

    auto built = edit::makeSetWheels(*project_, {sequenceId_, track_}, clip_, wheels);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    applyToWidgets();
    emit edited();
}

void EffectControls::pushRole() {
    if (updating_ || commands_ == nullptr || !clip_.isValid()) {
        return;
    }
    const auto role = static_cast<model::AudioRole>(role_->currentData().toInt());
    auto built = edit::makeSetAudioRole(*project_, {sequenceId_, track_}, clip_, role);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    applyToWidgets();
    emit edited();
}

/// Write the automation somebody would otherwise have drawn.
///
/// Reported through the panel rather than a dialog: the answer is visible on
/// the timeline the moment it lands, and a clip with no speech over it is not
/// an error -- it is a clip nothing was in the way of.
void EffectControls::duckUnderDialogue() {
    const model::Clip* clip = selectedClip();
    const model::Sequence* sequence =
        project_ != nullptr ? project_->findSequence(sequenceId_) : nullptr;
    if (clip == nullptr || sequence == nullptr || audio_ == nullptr || commands_ == nullptr) {
        return;
    }
    auto curve = render::duckingCurve(*sequence, *clip, *audio_);
    if (!curve) {
        return;
    }
    auto built =
        edit::makeSetCurve(*project_, {sequenceId_, track_}, clip_, model::Param::GainDb, *curve);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    applyToWidgets();
    emit keyframesChanged();
    emit edited();
}

void EffectControls::pushKeyer() {
    if (updating_ || commands_ == nullptr || !clip_.isValid()) {
        return;
    }
    model::Keyer keyer;
    keyer.kind = static_cast<model::KeyKind>(keyKind_->currentData().toInt());
    keyer.red = keyRed_->value();
    keyer.green = keyGreen_->value();
    keyer.blue = keyBlue_->value();
    keyer.tolerance = keyTolerance_->value();
    keyer.softness = keySoftness_->value();
    keyer.spill = keySpill_->value();
    keyer.lumaLow = keyLumaLow_->value();
    // A window that ends before it starts is refused by the operation, which
    // would leave the panel showing a value the clip does not have. Dragging
    // the bottom past the top pushes the top instead.
    keyer.lumaHigh = std::max(keyLumaHigh_->value(), keyer.lumaLow);
    keyer.showMatte = keyShowMatte_->isChecked();

    auto built = edit::makeSetKeyer(*project_, {sequenceId_, track_}, clip_, keyer);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    applyToWidgets();
    emit edited();
}

/// Turn the shape into the path that draws it.
///
/// One click, and the picture does not change: the converted path covers the
/// same pixels the rectangle or ellipse did, which is what makes it safe to
/// offer without a warning. What changes is that the outline now has points on
/// it somebody can drag.
void EffectControls::pushResponsive() {
    const model::Clip* clip = selectedClip();
    if (updating_ || clip == nullptr || commands_ == nullptr) {
        return;
    }
    // Whole frames: a protected stretch that ended between two frames would
    // be a boundary nothing on the timeline could line up with.
    const auto rate = clip->sourceRange.duration().rate();
    const auto frames = [&rate](double seconds) {
        return time::RationalTime{
            static_cast<std::int64_t>(std::llround(seconds * rate.toDouble())), rate};
    };
    auto built =
        edit::makeSetResponsive(*project_, {sequenceId_, track_}, clip_,
                                frames(introSeconds_->value()), frames(outroSeconds_->value()));
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    emit edited();
}

void EffectControls::setDrawingMask(bool drawing) {
    if (maskDraw_->isChecked() != drawing) {
        maskDraw_->setChecked(drawing);
    }
}

void EffectControls::convertMaskToPath() {
    const model::Clip* selected = selectedClip();
    if (selected == nullptr || commands_ == nullptr || !selected->mask.isSet()) {
        return;
    }
    model::Mask converted = selected->mask;
    converted.path = render::pathForShape(selected->mask);
    if (!converted.path.isSet()) {
        return;
    }
    converted.shape = model::MaskShape::Path;

    auto built = edit::makeSetMask(*project_, {sequenceId_, track_}, clip_, converted);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    applyToWidgets();
    emit edited();
}

void EffectControls::pushMask() {
    if (updating_ || commands_ == nullptr || !clip_.isValid()) {
        return;
    }
    model::Mask mask;
    mask.shape = static_cast<model::MaskShape>(maskShape_->currentData().toInt());
    mask.width = maskWidth_->value();
    mask.height = maskHeight_->value();
    mask.centreX = maskX_->value();
    mask.centreY = maskY_->value();
    mask.cornerRadius = maskCorner_->value();
    mask.feather = maskFeather_->value();
    mask.inverted = maskInverted_->isChecked();

    auto built = edit::makeSetMask(*project_, {sequenceId_, track_}, clip_, mask);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    emit edited();
}

void EffectControls::pushGraphic() {
    if (updating_ || commands_ == nullptr || !clip_.isValid()) {
        return;
    }
    const model::Clip* clip = selectedClip();
    if (clip == nullptr || !clip->graphic.isSet()) {
        return;
    }
    model::Graphic graphic = clip->graphic;
    graphic.kind = static_cast<model::GraphicKind>(shapeKind_->currentData().toInt());
    graphic.width = shapeWidth_->value();
    graphic.height = shapeHeight_->value();
    graphic.cornerRadius = shapeCorner_->value();
    graphic.feather = shapeFeather_->value();
    graphic.red = shapeRed_->value();
    graphic.green = shapeGreen_->value();
    graphic.blue = shapeBlue_->value();

    auto built = edit::makeSetGraphic(*project_, {sequenceId_, track_}, clip_, graphic);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    emit edited();
}

void EffectControls::pushLut(const model::LutRef& lut) {
    if (commands_ == nullptr || !clip_.isValid()) {
        return;
    }
    auto built = edit::makeSetLut(*project_, {sequenceId_, track_}, clip_, lut);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    applyToWidgets();
    emit edited();
}

void EffectControls::pushSecondary() {
    if (updating_ || commands_ == nullptr || !clip_.isValid()) {
        return;
    }
    const model::Secondary secondary = secondaryFromWidgets();
    auto built = edit::makeSetSecondary(*project_, {sequenceId_, track_}, clip_, secondary);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    hueBand_->setWindow(secondary.qualifier.hueCentre, secondary.qualifier.hueWidth,
                        secondary.qualifier.hueSoftness);
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
