#include "EffectControls.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/model/ColorCorrection.h"
#include "zaro/core/render/BakeLut.h"
#include "zaro/core/render/Ducking.h"
#include "zaro/core/render/PathRaster.h"

#include "Icons.h"
#include "Say.h"
#include "Theme.h"

namespace zaro::app {
namespace {

/// The width a spin box needs to show any value in [minimum, maximum] whole,
/// with its suffix.
///
/// A field that shrinks with the panel eventually cuts the unit off, and
/// "0.00 E" reads as a different number rather than as a shorter label -- which
/// has now happened twice, once to " stops" and once to " EV".
int widthForRange(const QDoubleSpinBox* spin, double minimum, double maximum, int decimals,
                  const QString& suffix, int pad = 34) {
    QString widest = QString::number(minimum, 'f', decimals) + suffix;
    if (QString::number(maximum, 'f', decimals).size() > widest.size()) {
        widest = QString::number(maximum, 'f', decimals) + suffix;
    }
    return spin->fontMetrics().horizontalAdvance(widest) + pad;
}

/// What a value field needs around its text once the steppers are gone: the
/// frame, and enough air that a digit does not touch it.
constexpr int kFieldPad = 16;

/// Which of the panel's groups a kind of clip has anything to say through.
///
/// The panel used to answer this with four booleans set while the clip was
/// read -- is it picture, is it sound, is it generated, can it be masked --
/// which meant every new kind of clip was another boolean, set in one place
/// and read in another, and that the answer for a title was assembled rather
/// than stated. Here it is stated: one row per kind, and adding a kind is a
/// row.
///
/// The judgements worth defending:
///
///   * A shape and a title have no *key*. A keyer measures what a camera saw
///     and pulls a matte from it; a generated picture is already exactly as
///     transparent as somebody authored it to be, and a control that can only
///     make it wrong is not a control.
///   * They have no *secondary* either. A secondary corrects the part of a
///     picture a qualifier selects, and the part of a flat fill a qualifier
///     selects is all of it or none of it -- the colour picker above it is the
///     same edit, made once.
///   * An adjustment layer keeps opacity and blend but loses *position, scale,
///     rotation and anchor*. It has no picture of its own; moving it would
///     move the correction off the thing it was made for. Its opacity is how
///     strong the correction is, which is exactly what somebody wants.
///   * An adjustment layer keeps its *mask*, because limiting a correction to
///     part of the frame is most of what one is for.
struct GroupSet {
    /// The Motion group as a whole.
    bool motion{false};
    /// The four rows inside it that place a picture in the frame. Separate
    /// from `motion` because opacity and blend outlive them.
    bool placement{false};
    /// The rows inside Motion that need a file behind the clip: pinning,
    /// stabilisation, retiming.
    bool mediaMotion{false};
    /// The graphic's box and fill. Shown for a title too -- a title has a box
    /// and a colour, and they are the same fields.
    bool graphic{false};
    bool text{false};
    bool mask{false};
    bool colour{false};
    bool secondary{false};
    bool key{false};
    bool effects{false};
    bool audio{false};
    /// The cameras of a multicam clip.
    bool angles{false};
    /// The way into the sequence a nested clip is made of.
    bool nested{false};
};

[[nodiscard]] GroupSet groupsFor(model::ClipKind kind) {
    GroupSet set;
    switch (kind) {
        case model::ClipKind::AudioMedia:
            set.audio = true;
            return set;
        case model::ClipKind::VideoMedia:
            set.motion = set.placement = set.mediaMotion = true;
            set.mask = set.colour = set.secondary = set.key = set.effects = true;
            return set;
        case model::ClipKind::Multicam:
            set.motion = set.placement = set.mediaMotion = true;
            set.mask = set.colour = set.secondary = set.key = set.effects = true;
            set.angles = true;
            return set;
        case model::ClipKind::Nested:
            // Everything a video clip gets except the parts that need a file:
            // there are no frames of a nested sequence to analyse, and the
            // operations that would say so already refuse it.
            set.motion = set.placement = true;
            set.mask = set.colour = set.secondary = set.key = set.effects = true;
            set.nested = true;
            return set;
        case model::ClipKind::Shape:
            set.motion = set.placement = true;
            set.graphic = set.mask = set.colour = set.effects = true;
            return set;
        case model::ClipKind::Text:
            set.motion = set.placement = true;
            set.graphic = set.text = set.mask = set.colour = set.effects = true;
            return set;
        case model::ClipKind::Adjustment:
            set.motion = true;
            set.mask = set.colour = set.secondary = set.effects = true;
            return set;
    }
    return set;
}

/// The four rows an adjustment layer does not have. See `GroupSet::placement`.
constexpr model::Param kPlacementParams[] = {
    model::Param::PositionX, model::Param::PositionY,       model::Param::ScaleX,
    model::Param::ScaleY,    model::Param::RotationDegrees, model::Param::AnchorX,
    model::Param::AnchorY,
};

/// What a clip says a parameter is at a moment, for the rows the panel shows.
///
/// The panel has no generic reader -- each group asks the clip its own way,
/// `transformAt` and `colorAt` and `gainDbAt` -- and comparing two clips row by
/// row needs one. Anything not on a row answers with its own value from the
/// primary group it belongs to; the parameters with no row here never reach it.
[[nodiscard]] double parameterAt(const model::Clip& clip, model::Param param,
                                 const time::RationalTime& at) {
    switch (param) {
        case model::Param::GainDb:
            return clip.gainDbAt(at);
        case model::Param::Pan:
            return clip.panAt(at);
        case model::Param::Temperature:
        case model::Param::Tint:
        case model::Param::Exposure:
        case model::Param::Contrast:
        case model::Param::Saturation: {
            const model::ColorCorrection colour = clip.colorAt(at);
            switch (param) {
                case model::Param::Temperature:
                    return colour.temperature;
                case model::Param::Tint:
                    return colour.tint;
                case model::Param::Exposure:
                    return colour.exposure;
                case model::Param::Contrast:
                    return colour.contrast;
                default:
                    return colour.saturation;
            }
        }
        case model::Param::TextReveal:
            // Not on the transform and not on the colour: the only parameter
            // whose value lives nowhere but its own curve, and whose value
            // without one is all of the text.
            return clip.parameterAt(model::Param::TextReveal, at);
        default:
            break;
    }
    const model::Transform transform = clip.transformAt(at);
    switch (param) {
        case model::Param::PositionX:
            return transform.positionX;
        case model::Param::PositionY:
            return transform.positionY;
        case model::Param::ScaleX:
            return transform.scaleX;
        case model::Param::ScaleY:
            return transform.scaleY;
        case model::Param::RotationDegrees:
            return transform.rotationDegrees;
        case model::Param::AnchorX:
            return transform.anchorX;
        case model::Param::AnchorY:
            return transform.anchorY;
        case model::Param::Opacity:
            return transform.opacity;
        default:
            return 0.0;
    }
}

[[nodiscard]] bool isPlacementParam(model::Param param) {
    return std::find(std::begin(kPlacementParams), std::end(kPlacementParams), param) !=
           std::end(kPlacementParams);
}

/// A slider that does not move when the panel is scrolled past it.
///
/// The same hazard `makeSpin` guards against, and the more dangerous half of
/// it: a spin box under the pointer at least shows what it changed, while a
/// slider nudged during a scroll looks like nothing happened until the picture
/// is wrong. Qt gives a slider wheel focus by default, so this says otherwise.
class ParamSlider : public QSlider {
public:
    explicit ParamSlider(QWidget* parent) : QSlider(Qt::Horizontal, parent) {
        setFocusPolicy(Qt::StrongFocus);
    }

protected:
    void wheelEvent(QWheelEvent* event) override {
        if (!hasFocus()) {
            event->ignore();
            return;
        }
        QSlider::wheelEvent(event);
    }
};

/// How many steps a parameter slider has between its ends.
///
/// Finer than the panel is wide at any size it docks at, so a drag never
/// quantises visibly, and coarse enough that an arrow key moves the value by
/// something a person can see.
constexpr int kSliderSteps = 1000;

/// Where on a slider spanning [lo, hi] the value `value` sits.
///
/// Pinned to an end rather than left where it was when the value is outside the
/// span: a knob that stayed put while the number moved would be describing a
/// different value.
int stepFor(double value, double lo, double hi) {
    const double where = hi > lo ? (value - lo) / (hi - lo) : 0.0;
    return static_cast<int>(std::lround(std::clamp(where, 0.0, 1.0) * kSliderSteps));
}

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

    // Wide enough for the largest value it can hold. `addRow` narrows this
    // again for a parameter whose slider covers less than its spin box allows:
    // the field only has to fit the values it will actually show.
    spin->setMinimumWidth(widthForRange(spin, minimum, maximum, decimals, suffix));
    return spin;
}

}  // namespace

EffectControls::EffectControls(QWidget* parent) : QWidget{parent} {
    // In the order the panel is built, which is not the order it is read: the
    // colour group is opened early and finished late, because the wheels and
    // the vignette belong in it but are made after the groups that follow it.
    // Kept that way deliberately -- the rows land in the same places, and the
    // widgets are created in the same order, so this is a move and not a
    // rearrangement.
    buildHeader();
    createParameterWidgets();
    buildMotionGroup();
    QFormLayout* colourForm = buildColourGroup();
    buildSecondaryGroup();
    buildMaskGroup();
    buildEffectsGroup();
    addWheelsTo(colourForm);
    addVignetteTo(colourForm);
    buildKeyGroup();
    buildGraphicGroup();
    buildTextGroup();
    buildAnglesGroup();
    buildNestedGroup();
    buildTrackGroup();
    buildProcessingGroup();
    buildAudioGroup();
    buildInfoGroup();
    assemblePanel();
}

void EffectControls::createParameterWidgets() {
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
}

void EffectControls::buildMotionGroup() {
    auto* motion = new QGroupBox("Motion", this);
    auto* motionForm = new QFormLayout(motion);
    // The spans the sliders cover, where that is not the whole range the spin
    // box accepts. See `SliderSpan`: these ranges are what somebody drags
    // across, and the field beside each one still takes anything.
    //
    // Position and anchor reach a frame off either side of a 4K timeline, which
    // is as far as a clip can go and still be partly on screen. Scale stops at
    // 4x, and a flip is typed rather than dragged to -- there is no travel
    // between -1 and 1 worth spending half the control on. Rotation is one turn
    // each way; the spin box winds on past it for an animation that spins.
    constexpr SliderSpan kFrameSpan{-4000.0, 4000.0};
    constexpr SliderSpan kScaleSpan{0.0, 4.0};
    constexpr SliderSpan kTurnSpan{-360.0, 360.0};

    addRow(motionForm, "Position X", model::Param::PositionX, positionX_, kFrameSpan);
    addRow(motionForm, "Position Y", model::Param::PositionY, positionY_, kFrameSpan);
    addRow(motionForm, "Scale X", model::Param::ScaleX, scaleX_, kScaleSpan);
    addRow(motionForm, "Scale Y", model::Param::ScaleY, scaleY_, kScaleSpan);
    addRow(motionForm, "Rotation", model::Param::RotationDegrees, rotation_, kTurnSpan);
    addRow(motionForm, "Anchor X", model::Param::AnchorX, anchorX_, kFrameSpan);
    addRow(motionForm, "Anchor Y", model::Param::AnchorY, anchorY_, kFrameSpan);
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
    pinRow_ = new QWidget(this);
    auto* pinRow = new QHBoxLayout(pinRow_);
    pinRow->setContentsMargins(0, 0, 0, 0);
    pinRow->addWidget(pin_);
    pinRow->addWidget(unpin_);
    motionForm->addRow(pinRow_);
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
    // Remembered so `applyPaneVisibility` can hide the whole run of media-only
    // rows together. Auto-reframe follows what is *in* a shot; there is nothing
    // in a rectangle to follow.
    motionMediaForm_ = motionForm;

    stabilise_ = new QPushButton("Stabilise", this);
    stabilise_->setObjectName("stabilise");
    stabilise_->setToolTip("Analyse this clip's own frames and hold the picture still");
    unstabilise_ = new QPushButton("Clear", this);
    unstabilise_->setObjectName("stabilise-clear");
    unstabilise_->setToolTip("Throw the stabilisation away and put the framing back");
    stabiliseRow_ = new QWidget(this);
    auto* stabiliseRow = new QHBoxLayout(stabiliseRow_);
    stabiliseRow->setContentsMargins(0, 0, 0, 0);
    stabiliseRow->addWidget(stabilise_);
    stabiliseRow->addWidget(unstabilise_);
    motionForm->addRow(stabiliseRow_);
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
    // Constant speed, above the varying kind. A clip plays at one rate or it
    // plays at a curve, and the two controls sit together because that is one
    // decision made two ways.
    speed_ = makeSpin(1.0, 10000.0, 5.0, 1, " %");
    speed_->setObjectName("clip-speed");
    speed_->setToolTip("How fast this clip plays. Whatever follows it on the track moves to suit");
    reverse_ = new QCheckBox("Reverse", this);
    reverse_->setObjectName("clip-reverse");
    reverse_->setToolTip("Play the clip backwards");
    speedRow_ = new QWidget(this);
    auto* speedRow = new QHBoxLayout(speedRow_);
    speedRow->setContentsMargins(0, 0, 0, 0);
    speedRow->addWidget(speed_, 1);
    speedRow->addWidget(reverse_);
    motionForm->addRow("Speed", speedRow_);
    connect(speed_, &QDoubleSpinBox::valueChanged, this, [this] { pushSpeed(); });
    connect(reverse_, &QCheckBox::toggled, this, [this] { pushSpeed(); });

    remapRow_ = new QWidget(this);
    auto* remapRow = new QHBoxLayout(remapRow_);
    remapRow->setContentsMargins(0, 0, 0, 0);
    remapRow->addWidget(timeRemap_);
    remapRow->addWidget(freeze_);
    motionForm->addRow(remapRow_);
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
    motion->setObjectName("inspector-group-motion");
}

QFormLayout* EffectControls::buildColourGroup() {
    auto* colour = new QGroupBox("Colour", this);
    colourGroup_ = colour;
    colour->setObjectName("inspector-group-colour");
    auto* colourForm = new QFormLayout(colour);
    colourForm_ = colourForm;
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
    connect(curves_, &CurveEditor::colorCurvesChanged, this,
            [this](const model::ColorCurves& changed, bool committed) {
                pushColorCurves(changed, committed);
            });

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
    lutRow_ = lutRow;
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
    return colourForm;
}

void EffectControls::buildSecondaryGroup() {
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
    secondary->setObjectName("inspector-group-secondary");

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
}

void EffectControls::buildMaskGroup() {
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
    maskBox->setObjectName("inspector-group-mask");

    for (QDoubleSpinBox* spin :
         {maskWidth_, maskHeight_, maskX_, maskY_, maskCorner_, maskFeather_}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { pushMask(); });
    }
    connect(maskShape_, &QComboBox::currentIndexChanged, this, [this] { pushMask(); });
    connect(maskInverted_, &QCheckBox::toggled, this, [this] { pushMask(); });
}

void EffectControls::buildEffectsGroup() {
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
    effectBox->setObjectName("inspector-group-effects");

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
}

void EffectControls::addWheelsTo(QFormLayout* colourForm) {
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
    wheelsBox_ = wheelsBox;
    colourForm->addRow(wheelsBox);
}

void EffectControls::addVignetteTo(QFormLayout* colourForm) {
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
}

void EffectControls::buildKeyGroup() {
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
    keyBox->setObjectName("inspector-group-key");

    for (QDoubleSpinBox* spin : {keyRed_, keyGreen_, keyBlue_, keyTolerance_, keySoftness_,
                                 keySpill_, keyLumaLow_, keyLumaHigh_}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { pushKeyer(); });
    }
    connect(keyKind_, &QComboBox::currentIndexChanged, this, [this] { pushKeyer(); });
    connect(keyShowMatte_, &QCheckBox::toggled, this, [this] { pushKeyer(); });
}

void EffectControls::buildGraphicGroup() {
    // A generated picture's box and fill. Shown for a clip that is one --
    // shape or title, since a title has a box and a colour and they are the
    // same fields. The controls are meaningless on a clip with media, and a
    // panel full of inert fields is worse than one that says nothing.
    shapeKind_ = new QComboBox(this);
    shapeKind_->setObjectName("shape-kind");
    // Text is in the list even though the Kind row is hidden for a title, so
    // that `findData` finds it. Without it the combo went to index -1 on a
    // title and the next edit to any field read that back as GraphicKind::None
    // -- which unset the graphic and deleted the title.
    for (const model::GraphicKind kind :
         {model::GraphicKind::Rectangle, model::GraphicKind::Ellipse, model::GraphicKind::Text}) {
        shapeKind_->addItem(QString::fromUtf8(model::toString(kind)), static_cast<int>(kind));
    }
    shapeWidth_ = makeSpin(0.0, 20000.0, 10.0, 1, " px");
    shapeHeight_ = makeSpin(0.0, 20000.0, 10.0, 1, " px");
    shapeCorner_ = makeSpin(0.0, 5000.0, 5.0, 1, " px");
    shapeFeather_ = makeSpin(0.0, 2000.0, 2.0, 1, " px");
    shapeRed_ = makeSpin(0.0, 1.0, 0.05, 3);
    shapeGreen_ = makeSpin(0.0, 1.0, 0.05, 3);
    shapeBlue_ = makeSpin(0.0, 1.0, 0.05, 3);
    shapeAlpha_ = makeSpin(0.0, 1.0, 0.05, 3);
    shapeAlpha_->setObjectName("shape-alpha");
    shapeAlpha_->setToolTip("How much of the shape is there, rather than how bright it is");

    auto* graphic = new QGroupBox("Shape", this);
    auto* graphicForm = new QFormLayout(graphic);
    graphicForm_ = graphicForm;
    graphicForm->addRow("Kind", shapeKind_);
    graphicForm->addRow("Width", shapeWidth_);
    graphicForm->addRow("Height", shapeHeight_);
    graphicForm->addRow("Corner", shapeCorner_);
    graphicForm->addRow("Feather", shapeFeather_);
    graphicForm->addRow("Red", shapeRed_);
    graphicForm->addRow("Green", shapeGreen_);
    graphicForm->addRow("Blue", shapeBlue_);
    graphicForm->addRow("Alpha", shapeAlpha_);

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
    graphic->setObjectName("inspector-group-shape");

    for (QDoubleSpinBox* spin : {shapeWidth_, shapeHeight_, shapeCorner_, shapeFeather_, shapeRed_,
                                 shapeGreen_, shapeBlue_, shapeAlpha_}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { pushGraphic(); });
    }
    connect(shapeKind_, &QComboBox::currentIndexChanged, this, [this] { pushGraphic(); });
}

void EffectControls::buildTextGroup() {
    // What a title says, and in what. The box it says it in is the graphic
    // group above, which a title shares with a shape.
    auto* text = new QGroupBox("Text", this);
    auto* form = new QFormLayout(text);

    textBody_ = new QPlainTextEdit(this);
    textBody_->setObjectName("text-body");
    textBody_->setTabChangesFocus(true);
    textBody_->setPlaceholderText("Title");
    // Four lines: enough for a lower third without taking the panel over, and
    // it scrolls past that. A field that grew with the text would move every
    // control under it while somebody was typing.
    textBody_->setFixedHeight(textBody_->fontMetrics().lineSpacing() * 4 + 12);

    textFamily_ = new QFontComboBox(this);
    textFamily_->setObjectName("text-family");
    textSize_ = makeSpin(1.0, 2000.0, 1.0, 1, " pt");
    textSize_->setObjectName("text-size");
    textBold_ = new QCheckBox("Bold", this);
    textBold_->setObjectName("text-bold");
    textItalic_ = new QCheckBox("Italic", this);
    textItalic_->setObjectName("text-italic");
    textAlign_ = new QComboBox(this);
    textAlign_->setObjectName("text-align");
    textAlign_->addItem("Left", -1);
    textAlign_->addItem("Centre", 0);
    textAlign_->addItem("Right", 1);

    form->addRow(textBody_);
    form->addRow("Font", textFamily_);

    // What happens when a project arrives from a machine with a different set
    // of fonts. The family is stored as a name and resolved when the text is
    // drawn -- deliberately, so the title comes back to itself on a machine
    // that has the typeface -- but the substitution is otherwise silent, and a
    // silent substitution is a delivery somebody signs off without noticing.
    textFontNote_ = new QLabel(this);
    textFontNote_->setObjectName("text-font-note");
    textFontNote_->setWordWrap(true);
    textFontNote_->setProperty("muted", true);
    textFontNote_->hide();
    form->addRow(textFontNote_);
    form->addRow("Size", textSize_);
    auto* styleRow = new QWidget(this);
    auto* style = new QHBoxLayout(styleRow);
    style->setContentsMargins(0, 0, 0, 0);
    style->addWidget(textBold_);
    style->addWidget(textItalic_);
    style->addStretch(1);
    form->addRow(styleRow);
    form->addRow("Align", textAlign_);

    // A typewriter, as a parameter rather than an effect: it animates with the
    // same stopwatch as everything else, and a title showing all of itself is
    // the value it has when nobody has touched it.
    textReveal_ = makeSpin(0.0, 1.0, 0.05, 3);
    textReveal_->setObjectName("text-reveal");
    textReveal_->setToolTip("How much of the line is shown. Animate it for a typewriter.");
    addRow(form, "Reveal", model::Param::TextReveal, textReveal_);
    textGroup_ = text;
    text->setObjectName("inspector-group-text");

    // On focus out and not on every keystroke. A command per character would
    // be a hundred undo steps for one line of a title, and re-reading the
    // model after each one would fight the cursor for where it is.
    connect(textBody_, &QPlainTextEdit::textChanged, this, [this] {
        if (updating_) {
            return;
        }
        textDirty_ = true;
    });
    textBody_->installEventFilter(this);

    connect(textFamily_, &QFontComboBox::currentFontChanged, this, [this] { pushText(); });
    connect(textSize_, &QDoubleSpinBox::valueChanged, this, [this] { pushText(); });
    connect(textBold_, &QCheckBox::toggled, this, [this] { pushText(); });
    connect(textItalic_, &QCheckBox::toggled, this, [this] { pushText(); });
    connect(textAlign_, &QComboBox::currentIndexChanged, this, [this] { pushText(); });
}

void EffectControls::buildAnglesGroup() {
    auto* angles = new QGroupBox("Angles", this);
    angles->setObjectName("inspector-group-angles");
    auto* form = new QFormLayout(angles);

    angleList_ = new QListWidget(this);
    angleList_->setObjectName("angle-list");
    form->addRow(angleList_);

    angleSwitch_ = new QPushButton("Cut to this angle", this);
    angleSwitch_->setObjectName("angle-switch");
    angleSwitch_->setToolTip(
        "Cut the clip at the playhead; the part after it plays the chosen angle");
    form->addRow(angleSwitch_);

    // Seconds rather than frames. An angle's offset is how far into its own
    // material the group's zero point is, and that is worked out from timecode
    // or by ear -- neither of which is counted in the sequence's frames.
    angleOffset_ = makeSpin(-86400.0, 86400.0, 0.1, 3, " s");
    angleOffset_->setObjectName("angle-offset");
    angleOffset_->setToolTip("How far into this camera's own material the group's zero point is");
    form->addRow("Offset", angleOffset_);

    anglesGroup_ = angles;

    connect(angleList_, &QListWidget::currentRowChanged, this, [this] { showAngles(); });
    connect(angleSwitch_, &QPushButton::clicked, this, [this] { switchToAngle(); });
    connect(angleOffset_, &QDoubleSpinBox::valueChanged, this, [this] { pushAngleOffset(); });
}

void EffectControls::buildNestedGroup() {
    auto* nested = new QGroupBox("Sequence", this);
    nested->setObjectName("inspector-group-nested");
    auto* form = new QFormLayout(nested);

    nestedName_ = new QLabel(QString::fromUtf8("\u2014"), this);
    nestedName_->setObjectName("nested-name");
    nestedName_->setWordWrap(true);
    form->addRow("Made of", nestedName_);

    nestedOpen_ = new QPushButton("Open it", this);
    nestedOpen_->setObjectName("nested-open");
    nestedOpen_->setToolTip("Show the sequence this clip is made of, so it can be edited");
    form->addRow(nestedOpen_);
    nestedGroup_ = nested;

    connect(nestedOpen_, &QPushButton::clicked, this, [this] {
        const model::Clip* clip = selectedClip();
        if (clip != nullptr && clip->nested.isValid()) {
            emit openSequenceRequested(clip->nested);
        }
    });
}

void EffectControls::buildTrackGroup() {
    auto* track = new QGroupBox("Track", this);
    track->setObjectName("inspector-group-track");
    auto* form = new QFormLayout(track);

    trackName_ = new QLineEdit(this);
    trackName_->setObjectName("track-name");
    trackName_->setPlaceholderText("Unnamed");
    // What the track is *for* -- "Dialogue", "B-roll" -- as distinct from V1
    // or A2, which say where it sits and are not stored at all.
    trackName_->setToolTip("What this track is for. V1 and A2 say where it sits, and are fixed");
    form->addRow("Name", trackName_);

    trackKind_ = new QLabel(QString::fromUtf8("\u2014"), this);
    trackKind_->setObjectName("track-kind");
    form->addRow("Kind", trackKind_);

    trackMuted_ = new QCheckBox("Muted", this);
    trackMuted_->setObjectName("track-muted");
    form->addRow(trackMuted_);

    trackLocked_ = new QCheckBox("Locked", this);
    trackLocked_->setObjectName("track-locked");
    trackLocked_->setToolTip("Nothing on this track can be moved, trimmed or changed");
    form->addRow(trackLocked_);

    trackSyncLocked_ = new QCheckBox("Sync lock", this);
    trackSyncLocked_->setObjectName("track-sync-locked");
    trackSyncLocked_->setToolTip("Ripple edits elsewhere move this track too, keeping it in sync");
    form->addRow(trackSyncLocked_);
    trackGroup_ = track;

    // On editing finished rather than per keystroke, for the reason a title's
    // words are: a command per character is one undo step per character, and
    // re-reading the model between each two fights the cursor.
    connect(trackName_, &QLineEdit::editingFinished, this, [this] { pushTrackName(); });
    connect(trackMuted_, &QCheckBox::toggled, this, [this] { pushTrackState(); });
    for (QCheckBox* box : {trackLocked_, trackSyncLocked_}) {
        connect(box, &QCheckBox::toggled, this, [this] { pushTrackLock(); });
    }

    // The mix side, on the Audio page. Level, balance and solo only: a track's
    // EQ and compressor have a purpose-built editor in the mixer, with a curve
    // to drag and a meter beside it, and a second set of numbered fields for
    // the same six values would be a second thing to keep in step.
    auto* level = new QGroupBox("Track level", this);
    level->setObjectName("inspector-group-track-level");
    auto* levelForm = new QFormLayout(level);
    trackGain_ = makeSpin(-96.0, 24.0, 0.5, 2, " dB");
    trackGain_->setObjectName("track-gain");
    trackPan_ = makeSpin(-1.0, 1.0, 0.05, 3);
    trackPan_->setObjectName("track-pan");
    trackSoloed_ = new QCheckBox("Solo", this);
    trackSoloed_->setObjectName("track-soloed");
    levelForm->addRow("Gain", trackGain_);
    levelForm->addRow("Pan", trackPan_);
    levelForm->addRow(trackSoloed_);
    trackLevelGroup_ = level;

    for (QDoubleSpinBox* spin : {trackGain_, trackPan_}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { pushTrackState(); });
    }
    connect(trackSoloed_, &QCheckBox::toggled, this, [this] { pushTrackState(); });
}

const model::Track* EffectControls::selectedTrack() const {
    if (project_ == nullptr || !trackSelection_.isValid()) {
        return nullptr;
    }
    const model::Sequence* sequence = project_->findSequence(sequenceId_);
    return sequence == nullptr ? nullptr : sequence->findTrack(trackSelection_);
}

void EffectControls::setTrackSelection(model::TrackId track) {
    commitText();
    trackSelection_ = track;
    if (track.isValid()) {
        // Exclusive with a clip selection: two things called "the selection"
        // is one too many, and the header can only name one of them.
        track_ = {};
        clip_ = {};
        others_.clear();
    }
    refresh();
}

void EffectControls::pushTrackState() {
    if (updating_ || commands_ == nullptr || !trackSelection_.isValid()) {
        return;
    }
    const model::Track* track = selectedTrack();
    if (track == nullptr) {
        return;
    }
    edit::TrackState state;
    state.muted = trackMuted_->isChecked();
    state.soloed = trackSoloed_->isChecked();
    state.gainDb = trackGain_->value();
    state.pan = trackPan_->value();
    auto built = edit::makeSetTrackState(*project_, sequenceId_, trackSelection_, state);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    emit edited();
}

void EffectControls::pushTrackLock() {
    if (updating_ || commands_ == nullptr || !trackSelection_.isValid()) {
        return;
    }
    auto built = edit::makeSetTrackLock(*project_, sequenceId_, trackSelection_,
                                        trackLocked_->isChecked(), trackSyncLocked_->isChecked());
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    emit edited();
}

void EffectControls::pushTrackName() {
    if (updating_ || commands_ == nullptr || !trackSelection_.isValid()) {
        return;
    }
    const model::Track* track = selectedTrack();
    if (track == nullptr || track->name() == trackName_->text().toStdString()) {
        return;
    }
    auto built = edit::makeRenameTrack(*project_, sequenceId_, trackSelection_,
                                       trackName_->text().toStdString());
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    emit edited();
}

void EffectControls::buildProcessingGroup() {
    auto* processing = new QGroupBox("Repair", this);
    processing->setObjectName("inspector-group-processing");
    auto* form = new QFormLayout(processing);

    // "Repair" rather than "EQ and compression", because that is what these
    // are for on a clip. The mix decision -- everything on this track gets the
    // same high pass -- belongs to the track, and has a channel strip of its
    // own in the mixer.
    clipEqOn_ = new QCheckBox("Filter", this);
    clipEqOn_->setObjectName("clip-eq-on");
    clipEqOn_->setToolTip("Take the bottom, the top, or one frequency out of this clip alone");
    form->addRow(clipEqOn_);

    // Hertz, wide open at both ends, and zero means "not filtering" rather
    // than "filtering at DC" -- which is what `AudioEq` already says a zero is.
    clipHighPass_ = makeSpin(0.0, 20000.0, 10.0, 0, " Hz");
    clipHighPass_->setObjectName("clip-highpass");
    clipHighPass_->setToolTip("Everything below this is removed. 0 leaves the bottom alone");
    clipLowPass_ = makeSpin(0.0, 20000.0, 10.0, 0, " Hz");
    clipLowPass_->setObjectName("clip-lowpass");
    clipLowPass_->setToolTip("Everything above this is removed. 0 leaves the top alone");
    clipPeakHz_ = makeSpin(20.0, 20000.0, 10.0, 0, " Hz");
    clipPeakHz_->setObjectName("clip-peak-hz");
    clipPeakGain_ = makeSpin(-24.0, 24.0, 0.5, 1, " dB");
    clipPeakGain_->setObjectName("clip-peak-gain");
    clipPeakQ_ = makeSpin(0.1, 12.0, 0.1, 2);
    clipPeakQ_->setObjectName("clip-peak-q");
    clipPeakQ_->setToolTip("How narrow the bell is. Higher is narrower");
    form->addRow("High pass", clipHighPass_);
    form->addRow("Low pass", clipLowPass_);
    form->addRow("Bell at", clipPeakHz_);
    form->addRow("Bell gain", clipPeakGain_);
    form->addRow("Bell Q", clipPeakQ_);

    clipCompressorOn_ = new QCheckBox("Compress", this);
    clipCompressorOn_->setObjectName("clip-compressor-on");
    clipCompressorOn_->setToolTip("Hold this clip down when it gets loud");
    form->addRow(clipCompressorOn_);

    clipThreshold_ = makeSpin(-60.0, 0.0, 0.5, 1, " dB");
    clipThreshold_->setObjectName("clip-threshold");
    clipRatio_ = makeSpin(1.0, 20.0, 0.5, 2, ":1");
    clipRatio_->setObjectName("clip-ratio");
    clipAttack_ = makeSpin(0.1, 200.0, 1.0, 1, " ms");
    clipAttack_->setObjectName("clip-attack");
    clipRelease_ = makeSpin(1.0, 2000.0, 10.0, 0, " ms");
    clipRelease_->setObjectName("clip-release");
    clipMakeup_ = makeSpin(-12.0, 24.0, 0.5, 1, " dB");
    clipMakeup_->setObjectName("clip-makeup");
    form->addRow("Threshold", clipThreshold_);
    form->addRow("Ratio", clipRatio_);
    form->addRow("Attack", clipAttack_);
    form->addRow("Release", clipRelease_);
    form->addRow("Makeup", clipMakeup_);

    processingGroup_ = processing;

    for (QDoubleSpinBox* spin :
         {clipHighPass_, clipLowPass_, clipPeakHz_, clipPeakGain_, clipPeakQ_, clipThreshold_,
          clipRatio_, clipAttack_, clipRelease_, clipMakeup_}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { pushProcessing(); });
    }
    for (QCheckBox* box : {clipEqOn_, clipCompressorOn_}) {
        connect(box, &QCheckBox::toggled, this, [this] { pushProcessing(); });
    }
}

void EffectControls::pushProcessing() {
    if (updating_ || commands_ == nullptr || !clip_.isValid()) {
        return;
    }
    model::AudioEq eq;
    eq.enabled = clipEqOn_->isChecked();
    eq.highPassHz = clipHighPass_->value();
    eq.lowPassHz = clipLowPass_->value();
    eq.peakHz = clipPeakHz_->value();
    eq.peakGainDb = clipPeakGain_->value();
    eq.peakQ = clipPeakQ_->value();

    model::Compressor compressor;
    compressor.enabled = clipCompressorOn_->isChecked();
    compressor.thresholdDb = clipThreshold_->value();
    compressor.ratio = clipRatio_->value();
    compressor.attackMs = clipAttack_->value();
    compressor.releaseMs = clipRelease_->value();
    compressor.makeupDb = clipMakeup_->value();

    applyToSelection([this, eq, compressor](model::TrackId track, model::ClipId id) {
        return edit::makeSetClipProcessing(*project_, {sequenceId_, track}, id, eq, compressor);
    });
    emit edited();
}

void EffectControls::buildAudioGroup() {
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
    audio->setObjectName("inspector-group-audio");
}

void EffectControls::assemblePanel() {
    // Scrolled, because the panel is now taller than a short display: motion,
    // colour, a curve editor, a secondary and audio. Without this the last
    // group is simply unreachable, with nothing on screen to suggest it exists.
    auto* inner = new QWidget;
    auto* layout = new QVBoxLayout(inner);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(enabled_);
    layout->addWidget(trackGroup_);
    layout->addWidget(trackLevelGroup_);
    layout->addWidget(videoGroup_);
    // Under Motion, because what a clip is made of comes before what has been
    // done to it, and above everything a grade touches.
    layout->addWidget(anglesGroup_);
    layout->addWidget(nestedGroup_);
    // Text above the box it sits in: what a title says is the thing somebody
    // came to the panel to change, and how wide it is is the thing they change
    // afterwards. A shape has no text group, so the order costs it nothing.
    layout->addWidget(textGroup_);
    layout->addWidget(graphicGroup_);
    layout->addWidget(maskGroup_);
    layout->addWidget(colourGroup_);
    layout->addWidget(secondaryGroup_);
    layout->addWidget(keyGroup_);
    layout->addWidget(effectGroup_);
    layout->addWidget(audioGroup_);
    layout->addWidget(processingGroup_);
    layout->addWidget(infoGroup_);
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
    outer->setSpacing(0);
    // Above the scroll area rather than inside it, so it stays put. See
    // `buildHeader`.
    outer->addWidget(tabBar_);
    outer->addWidget(identityRow_);
    outer->addWidget(scroll);

    // One width for every value field, so the sliders all end in the same place.
    // Sized for its own widest value, each field is a different size and the
    // column down the panel comes out ragged; widening the rest to match the
    // widest costs nothing, since the panel is already that wide because of it.
    //
    // Fixed rather than a minimum, because a minimum does not win. A spin box
    // asks for the width of the largest number its *range* can hold, and
    // position's range is ±100000px -- so the layout hands it 121px however
    // small a floor it is given, and the field `addRow` narrowed stays wide.
    int widest = 0;
    for (const Row& row : rows_) {
        widest = std::max(widest, row.spin->minimumWidth());
    }

    // And the panel is as wide as its widest row, said here rather than assumed
    // somewhere else.
    //
    // The scrollbar policy above promises the controls keep the width they
    // need. That only holds if nothing narrower is imposed from outside, and
    // something was: the window capped this panel at 330 while the rows asked
    // for 383. With horizontal scrolling deliberately off, the difference came
    // off the right-hand end of every value field -- "0.0 p" where "0.0 px" was
    // meant, and a rotation reading "0.00" with the degree sign cut away. At
    // every window size, since the cap did not depend on one.
    setMinimumWidth(inner->minimumSizeHint().width() +
                    scroll->verticalScrollBar()->sizeHint().width());

    for (const Row& row : rows_) {
        row.spin->setFixedWidth(widest);

        const model::Param param = row.param;
        connect(row.spin, &QDoubleSpinBox::valueChanged, this,
                [this, param](double value) { pushParameter(param, value); });
    }
    connect(blend_, &QComboBox::currentIndexChanged, this, [this] {
        if (updating_ || commands_ == nullptr || !clip_.isValid()) {
            return;
        }
        const auto mode = static_cast<model::BlendMode>(blend_->currentData().toInt());
        applyToSelection([this, mode](model::TrackId track, model::ClipId id) {
            return edit::makeSetBlendMode(*project_, {sequenceId_, track}, id, mode);
        });
        commands_->breakMerge();
        emit edited();
    });
    connect(enabled_, &QCheckBox::toggled, this, [this](bool on) {
        if (updating_ || commands_ == nullptr || !clip_.isValid()) {
            return;
        }
        applyToSelection([this, on](model::TrackId track, model::ClipId id) {
            return edit::makeSetClipEnabled(*project_, {sequenceId_, track}, id, on);
        });
        emit edited();
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
    // Before the selection moves, not after: the words in the field belong to
    // the clip being left, and a title typed and then clicked away from should
    // not depend on the field having been given up focus first. Focus-out is
    // the usual path; this is the one that catches everything else.
    commitText();
    track_ = track;
    clip_ = clip;
    others_.clear();
    trackSelection_ = {};
    refresh();
}

void EffectControls::setSelection(const std::vector<edit::ClipRef>& clips) {
    commitText();
    if (clips.empty()) {
        track_ = {};
        clip_ = {};
        others_.clear();
        refresh();
        return;
    }
    track_ = clips.front().track;
    clip_ = clips.front().clip;
    others_.assign(clips.begin() + 1, clips.end());
    // Picking clips turns a track selection off, the same way the timeline
    // treats the two as exclusive.
    trackSelection_ = {};
    refresh();
}

void EffectControls::applyToSelection(
    const std::function<Result<edit::CommandPtr>(model::TrackId, model::ClipId)>& build) {
    if (commands_ == nullptr || project_ == nullptr || !clip_.isValid()) {
        return;
    }
    // One undo step for the whole gesture. Merge keys cannot do this: they name
    // what is being changed, so five clips is five keys -- and undoing a change
    // to five clips a clip at a time is not what anybody meant by it.
    edit::CommandStack::Group group{*commands_};
    auto run = [&](model::TrackId track, model::ClipId clip) {
        auto built = build(track, clip);
        if (built) {
            commands_->execute(*project_, std::move(*built));
        }
        // A clip the edit does not apply to is skipped rather than aborting the
        // rest: a selection is not required to be uniform, and one clip that
        // cannot take a value is not a reason the other four should not.
    };
    run(track_, clip_);
    for (const edit::ClipRef& other : others_) {
        run(other.track, other.clip);
    }
}

bool EffectControls::selectionAgreesOn(model::Param param) const {
    if (others_.empty() || project_ == nullptr) {
        return true;
    }
    const model::Clip* primary = selectedClip();
    if (primary == nullptr) {
        return true;
    }
    const model::Sequence* sequence = project_->findSequence(sequenceId_);
    if (sequence == nullptr) {
        return true;
    }
    const double wanted = parameterAt(*primary, param, position_);
    for (const edit::ClipRef& other : others_) {
        const model::Track* track = sequence->findTrack(other.track);
        const model::Clip* clip = track == nullptr ? nullptr : track->find(other.clip);
        if (clip == nullptr) {
            continue;
        }
        // Compared at the playhead, like everything else the panel shows: two
        // clips with the same static opacity and different fades on them do not
        // agree about opacity *here*, which is the value in the box.
        if (std::fabs(parameterAt(*clip, param, position_) - wanted) > 1e-9) {
            return false;
        }
    }
    return true;
}

void EffectControls::markDisagreements() {
    for (const Row& row : rows_) {
        if (row.label == nullptr) {
            continue;
        }
        const bool agrees = selectionAgreesOn(row.param);
        // An asterisk rather than an emptied field: a blank box invites
        // somebody to think the value is zero, and the number that is there is
        // at least true of the clip the header names.
        const QString base = row.label->property("rowLabel").toString();
        row.label->setText(agrees ? base : base + " *");
        row.label->setToolTip(agrees ? QString{}
                                     : QString{"The selected clips do not agree about this. "
                                               "The value shown is %1's; changing it sets all of "
                                               "them."}
                                           .arg(identityNameFull_));
    }
}

const model::Clip* EffectControls::clipAt(model::TrackId track, model::ClipId clip) const {
    if (project_ == nullptr || !clip.isValid()) {
        return nullptr;
    }
    const model::Sequence* sequence = project_->findSequence(sequenceId_);
    if (sequence == nullptr) {
        return nullptr;
    }
    const model::Track* found = sequence->findTrack(track);
    return found != nullptr ? found->find(clip) : nullptr;
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
    // Every stage of the grade chain lives on the Inspector page, so a click on
    // a node in the Color workspace has to bring that page up first. Scrolling
    // to a hidden widget lands nowhere and reads as a dead node.
    setPane(Pane::Inspector);
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
        // After the page change, so the scroll area measures the layout it is
        // about to show rather than the one it was showing.
        scroll_->widget()->adjustSize();
        scroll_->ensureWidgetVisible(target, 0, 40);
    }
}

void EffectControls::setEditingEnabled(bool enabled) {
    enabled_->setEnabled(enabled);
    videoGroup_->setEnabled(enabled);
    graphicGroup_->setEnabled(enabled);
    textGroup_->setEnabled(enabled);
    anglesGroup_->setEnabled(enabled);
    nestedGroup_->setEnabled(enabled);
    maskGroup_->setEnabled(enabled);
    colourGroup_->setEnabled(enabled);
    secondaryGroup_->setEnabled(enabled);
    keyGroup_->setEnabled(enabled);
    effectGroup_->setEnabled(enabled);
    audioGroup_->setEnabled(enabled);
    processingGroup_->setEnabled(enabled);
}

void EffectControls::refresh() {
    applyToWidgets();
}

void EffectControls::applyToWidgets() {
    if (trackSelection_.isValid()) {
        applyTrack();
        return;
    }
    const model::Clip* clip = selectedClip();
    if (clip == nullptr) {
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
        angleList_->clear();
        nestedName_->setText(QString::fromUtf8("\u2014"));
        kind_.reset();
        curves_->setCurves(model::ToneCurves{});
        curves_->setColorCurves(model::ColorCurves{});
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
        const model::AudioEq noEq;
        const model::Compressor noCompressor;
        clipEqOn_->setChecked(false);
        clipHighPass_->setValue(noEq.highPassHz);
        clipLowPass_->setValue(noEq.lowPassHz);
        clipPeakHz_->setValue(noEq.peakHz);
        clipPeakGain_->setValue(noEq.peakGainDb);
        clipPeakQ_->setValue(noEq.peakQ);
        clipCompressorOn_->setChecked(false);
        clipThreshold_->setValue(noCompressor.thresholdDb);
        clipRatio_->setValue(noCompressor.ratio);
        clipAttack_->setValue(noCompressor.attackMs);
        clipRelease_->setValue(noCompressor.releaseMs);
        clipMakeup_->setValue(noCompressor.makeupDb);
        enabled_->setChecked(false);
        updating_ = false;
        applyIdentity();
        applyPaneVisibility();
        applyKeyframeButtons();
        return;
    }

    const model::Sequence* sequence = project_->findSequence(sequenceId_);
    const model::Track* track = sequence->findTrack(track_);
    const bool isVideo = track != nullptr && track->kind() == model::TrackKind::Video;

    setEditingEnabled(true);
    // What this clip *is*, asked once. Which groups that means, and which tab
    // has anything behind it, is `groupsFor` in `applyPaneVisibility`.
    kind_ = model::clipKindOf(*clip, isVideo ? model::TrackKind::Video : model::TrackKind::Audio);
    // A selection of mixed kinds shows what they have in common, which is what
    // any clip has: it is placed, it is graded, it plays or it does not. The
    // primary's kind would otherwise offer a shape's controls for a selection
    // of four camera clips and one rectangle.
    for (const edit::ClipRef& other : others_) {
        const model::Track* otherTrack = sequence->findTrack(other.track);
        const model::Clip* otherClip =
            otherTrack == nullptr ? nullptr : otherTrack->find(other.clip);
        if (otherClip == nullptr) {
            continue;
        }
        const model::ClipKind otherKind = model::clipKindOf(
            *otherClip, otherTrack->kind() == model::TrackKind::Video ? model::TrackKind::Video
                                                                      : model::TrackKind::Audio);
        if (otherKind != *kind_) {
            // Sound and picture share nothing the panel can write, so a mixed
            // selection keeps whichever the primary is and the other tab stays
            // dark. Two kinds of picture fall back to the plainest of them.
            if (model::hasPicture(otherKind) != model::hasPicture(*kind_)) {
                continue;
            }
            kind_ = model::hasPicture(*kind_) ? model::ClipKind::VideoMedia
                                              : model::ClipKind::AudioMedia;
        }
    }
    if (track != nullptr && track->isLocked()) {
        setEditingEnabled(false);
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

    if (clip->graphic.isSet()) {
        shapeKind_->setCurrentIndex(shapeKind_->findData(static_cast<int>(clip->graphic.kind)));
        shapeWidth_->setValue(clip->graphic.width);
        shapeHeight_->setValue(clip->graphic.height);
        shapeCorner_->setValue(clip->graphic.cornerRadius);
        shapeFeather_->setValue(clip->graphic.feather);
        shapeRed_->setValue(clip->graphic.red);
        shapeGreen_->setValue(clip->graphic.green);
        shapeBlue_->setValue(clip->graphic.blue);
        shapeAlpha_->setValue(clip->graphic.alpha);
        introSeconds_->setValue(clip->responsive.intro.toSecondsDouble());
        outroSeconds_->setValue(clip->responsive.outro.toSecondsDouble());

        // Only if it differs. Writing the same words back would move the
        // cursor to the start of the field, which is what a re-read triggered
        // by a keystroke elsewhere in the panel would do to somebody midway
        // through typing a title.
        const QString said = QString::fromStdString(clip->graphic.text);
        if (textBody_->toPlainText() != said) {
            textBody_->setPlainText(said);
        }
        textDirty_ = false;
        const QString wanted = QString::fromStdString(clip->graphic.family);
        textFamily_->setCurrentFont(QFont{wanted});
        // The combo lists what is installed, so a family this machine does not
        // have cannot be shown in it: it silently reads back as whichever font
        // Qt substituted. The note is what keeps the panel honest.
        const bool missing =
            !wanted.isEmpty() && !QFontDatabase::families().contains(wanted, Qt::CaseInsensitive);
        if (missing) {
            const QString instead = QFontInfo{QFont{wanted}}.family();
            textFontNote_->setText(
                QString("%1 is not on this machine \u2014 showing %2.").arg(wanted, instead));
        }
        textFontNote_->setVisible(missing);
        textSize_->setValue(clip->graphic.pointSize);
        textBold_->setChecked(clip->graphic.bold);
        textItalic_->setChecked(clip->graphic.italic);
        textReveal_->setValue(clip->parameterAt(model::Param::TextReveal, position_));
        const int alignIndex = textAlign_->findData(clip->graphic.alignment);
        textAlign_->setCurrentIndex(alignIndex >= 0 ? alignIndex : textAlign_->findData(0));
    }

    curves_->setCurves(clip->curves);
    curves_->setColorCurves(clip->colorCurves);
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

    // Read rather than stored: a clip's speed is the ratio between its two
    // ranges, and asking the clip is the only way to be sure the number here
    // agrees with the picture. A remapped clip has no single speed, so the
    // control says so by standing down rather than by showing one of them.
    speed_->setValue(clip->speed() * 100.0);
    reverse_->setChecked(clip->reversed);
    // A still has no speed to change and nothing to play backwards: it is one
    // picture, and its length is set by trimming it. The controls stand down
    // rather than disappearing, so the panel does not change shape depending on
    // which clip is selected.
    const model::MediaRef* source =
        project_ != nullptr ? project_->findMedia(clip->source) : nullptr;
    const bool still = source != nullptr && source->info.isStill();
    speed_->setEnabled(!clip->isTimeRemapped() && !still);
    reverse_->setEnabled(!clip->isTimeRemapped() && !still);

    if (clip->isMulticam()) {
        showAngles();
    }
    if (clip->nested.isValid()) {
        const model::Sequence* inner = project_->findSequence(clip->nested);
        nestedName_->setText(inner == nullptr ? QString::fromUtf8("\u2014")
                                              : QString::fromStdString(inner->name()));
        nestedOpen_->setEnabled(inner != nullptr);
    }

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
    clipEqOn_->setChecked(clip->eq.enabled);
    clipHighPass_->setValue(clip->eq.highPassHz);
    clipLowPass_->setValue(clip->eq.lowPassHz);
    clipPeakHz_->setValue(clip->eq.peakHz);
    clipPeakGain_->setValue(clip->eq.peakGainDb);
    clipPeakQ_->setValue(clip->eq.peakQ);
    clipCompressorOn_->setChecked(clip->compressor.enabled);
    clipThreshold_->setValue(clip->compressor.thresholdDb);
    clipRatio_->setValue(clip->compressor.ratio);
    clipAttack_->setValue(clip->compressor.attackMs);
    clipRelease_->setValue(clip->compressor.releaseMs);
    clipMakeup_->setValue(clip->compressor.makeupDb);
    enabled_->setChecked(clip->enabled);
    updating_ = false;
    applyIdentity();
    // The page a clip arrives on is the first one that has anything on it, so
    // selecting a sound clip while the Inspector tab is up does not show an
    // empty column with the answer one tab over.
    const GroupSet groups = kind_ ? groupsFor(*kind_) : GroupSet{};
    if ((pane_ == Pane::Inspector && !groups.motion) || (pane_ == Pane::Audio && !groups.audio)) {
        setPane(groups.audio ? Pane::Audio : Pane::Inspector);
    } else {
        applyPaneVisibility();
    }
    applySliders();
    applyKeyframeButtons();
    markDisagreements();
}

void EffectControls::applyTrack() {
    const model::Track* track = selectedTrack();
    if (track == nullptr) {
        // The track went away -- removed, or the panel was rebound to another
        // sequence. Fall back to having nothing selected rather than showing a
        // page about it.
        trackSelection_ = {};
        setEditingEnabled(false);
        applyIdentity();
        applyPaneVisibility();
        return;
    }
    setEditingEnabled(true);
    trackGroup_->setEnabled(true);
    trackLevelGroup_->setEnabled(true);

    updating_ = true;
    // Only when it differs, so a re-read does not take the cursor back to the
    // start of a name somebody is partway through typing.
    const QString named = QString::fromStdString(track->name());
    if (trackName_->text() != named) {
        trackName_->setText(named);
    }
    const bool sound = track->kind() == model::TrackKind::Audio;
    trackKind_->setText(sound ? "Sound" : "Picture");
    // What muting a track means depends on what is on it, and "muted" for a
    // picture track is a word for something nobody can hear.
    trackMuted_->setText(sound ? "Muted" : "Hidden");
    trackMuted_->setChecked(track->isMuted());
    trackLocked_->setChecked(track->isLocked());
    trackSyncLocked_->setChecked(track->isSyncLocked());
    trackSoloed_->setChecked(track->isSoloed());
    trackGain_->setValue(track->gainDb());
    trackPan_->setValue(track->pan());
    updating_ = false;

    applyIdentity();
    // A picture track's Audio page has nothing on it, so a selection that
    // arrives while that page is up is moved to the one that has.
    if (pane_ == Pane::Audio && !sound) {
        setPane(Pane::Inspector);
    } else {
        applyPaneVisibility();
    }
}

void EffectControls::addRow(QFormLayout* form, const QString& label, model::Param param,
                            QDoubleSpinBox* spin, std::optional<SliderSpan> span) {
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

    // The span the slider covers. The spin box's own range unless the caller
    // said otherwise, which it does for the parameters whose range is a guard
    // rail rather than something anybody drags across. See `SliderSpan`.
    const double lo = span ? span->lo : spin->minimum();
    const double hi = span ? span->hi : spin->maximum();

    auto* slider = new ParamSlider(this);
    slider->setRange(0, kSliderSteps);
    // Named for what it controls, as the two buttons above are, so a self-test
    // driving the real panel can find a row by its parameter rather than by
    // where it happens to sit in the layout.
    slider->setObjectName(QString{"slider:"} + model::toString(param));
    spin->setObjectName(QString{"spin:"} + model::toString(param));
    slider->setToolTip(label);
    // Enough travel to be worth dragging, and no more: the value beside it is
    // what the row is for, and a slider that pushed the number out of the panel
    // would have taken the readable half away to add the coarse one.
    slider->setMinimumWidth(56);

    // No steppers on a row that has a slider. They were how a value was nudged
    // without typing it, and that is what the slider and the arrow keys are for
    // now -- so they are twenty pixels of the panel spent twice on one job, and
    // the design draws a plain field.
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    // Narrowed to the values the row will actually show. Position is clamped at
    // ±100000px so a typo cannot corrupt a transform, and a field sized for
    // "-100000.0 px" is three quarters of the panel spent on a number nobody
    // will see.
    spin->setMinimumWidth(widthForRange(spin, lo, hi, spin->decimals(), spin->suffix(), kFieldPad));

    auto* line = new QWidget(this);
    auto* row = new QHBoxLayout(line);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);
    row->addWidget(stopwatch);
    row->addWidget(keyframe);
    row->addWidget(slider, 1);
    row->addWidget(spin);

    // The label keeps the width it needs. A form column is free to shrink to
    // whatever is left over, and "Position X" reading as "Position" next to
    // another row reading "Position" is worse than a narrow value field --
    // twice now a control has shipped with its name cut in half.
    auto* text = new QLabel(label, this);
    // Kept, because `markDisagreements` appends to it and has to be able to
    // take that back without having to know how to spell the name again.
    text->setProperty("rowLabel", label);
    // Room for the mark as well as the name, so putting one on does not move
    // the value column.
    text->setMinimumWidth(text->fontMetrics().horizontalAdvance(label + " *"));
    form->addRow(text, line);

    connect(stopwatch, &QToolButton::clicked, this,
            [this, param](bool on) { toggleAnimated(param, on); });
    connect(keyframe, &QToolButton::clicked, this, [this, param] { toggleKeyframe(param); });

    // The two halves of the row keep each other in step, and the spin box is
    // the half that talks to the model. A slider drag therefore takes the same
    // path a typed value does -- rounded and clamped the way the panel displays
    // it, pushed with the same merge key, so the whole drag is one undo step.
    connect(slider, &QSlider::valueChanged, this, [this, spin, lo, hi](int step) {
        if (syncing_) {
            return;
        }
        syncing_ = true;
        spin->setValue(lo + (hi - lo) * step / kSliderSteps);
        syncing_ = false;
    });
    connect(spin, &QDoubleSpinBox::valueChanged, this, [this, slider, lo, hi](double value) {
        if (syncing_) {
            return;
        }
        syncing_ = true;
        slider->setValue(stepFor(value, lo, hi));
        syncing_ = false;
    });

    rows_.push_back(Row{param, form, line, text, slider, spin, stopwatch, keyframe, lo, hi});
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

void EffectControls::applySliders() {
    for (const Row& row : rows_) {
        // Blocked rather than guarded by `syncing_`: this runs inside
        // `applyToWidgets`, and a slider that answered here would write the
        // model's own value back at it as though somebody had dragged it.
        QSignalBlocker block{row.slider};
        row.slider->setValue(stepFor(row.spin->value(), row.sliderLo, row.sliderHi));
    }
}

// --- The header -------------------------------------------------------------

void EffectControls::buildHeader() {
    // The same strip the bin has, because the design draws one strip and puts
    // it on both panels. `#bin-tab` styles the pill; sharing the rule is what
    // keeps the two from drifting a half pixel apart the next time either is
    // touched.
    tabBar_ = new QFrame(this);
    tabBar_->setObjectName("inspector-tabbar");
    tabBar_->setFixedHeight(34);
    auto* tabRow = new QHBoxLayout(tabBar_);
    tabRow->setContentsMargins(8, 0, 6, 0);
    tabRow->setSpacing(2);

    tabs_ = new QButtonGroup(this);
    tabs_->setExclusive(true);
    struct TabSpec {
        const char* label;
        const char* name;
        const char* tip;
        Pane pane;
        QPushButton** slot;
    };
    const TabSpec specs[] = {
        {"Inspector", "inspector-tab-inspector", "How this clip looks", Pane::Inspector,
         &inspectorTab_},
        {"Audio", "inspector-tab-audio", "How this clip sounds", Pane::Audio, &audioTab_},
        {"Info", "inspector-tab-info", "What this clip is", Pane::Info, &infoTab_},
    };
    for (const TabSpec& spec : specs) {
        auto* tab = new QPushButton(QString::fromUtf8(spec.label), tabBar_);
        tab->setObjectName(spec.name);
        tab->setProperty("class", "inspector-tab");
        tab->setCheckable(true);
        tab->setFocusPolicy(Qt::NoFocus);
        tab->setCursor(Qt::PointingHandCursor);
        tab->setToolTip(QString::fromUtf8(spec.tip));
        const Pane pane = spec.pane;
        connect(tab, &QPushButton::clicked, this, [this, pane] { setPane(pane); });
        tabs_->addButton(tab);
        tabRow->addWidget(tab);
        *spec.slot = tab;
    }
    inspectorTab_->setChecked(true);
    tabRow->addStretch(1);

    // The arrow the design puts at the end of the strip. It resets the page
    // that is showing and nothing else -- see `resetPane`, which says why that
    // is narrower than the tab it sits above.
    resetButton_ = new QPushButton(tabBar_);
    resetButton_->setObjectName("bin-glyph-button");
    resetButton_->setIcon(icons::toolIcon(icons::Glyph::Revert, 13));
    resetButton_->setFixedSize(26, 24);
    resetButton_->setFocusPolicy(Qt::NoFocus);
    connect(resetButton_, &QPushButton::clicked, this, [this] { resetPane(); });
    tabRow->addWidget(resetButton_);

    // --- the row that says which clip this is -----------------------------
    identityRow_ = new QFrame(this);
    identityRow_->setObjectName("inspector-identity");
    auto* identity = new QHBoxLayout(identityRow_);
    identity->setContentsMargins(12, 10, 12, 10);
    identity->setSpacing(10);

    // A glyph and not a thumbnail. A frame of the clip would want a decode, and
    // the header has to be right the instant the selection changes -- a tile
    // that fills in half a second later is a tile that is wrong half the time.
    identityTile_ = new QLabel(identityRow_);
    identityTile_->setObjectName("inspector-identity-tile");
    identityTile_->setFixedSize(52, 32);
    identityTile_->setAlignment(Qt::AlignCenter);
    identity->addWidget(identityTile_);

    auto* names = new QVBoxLayout;
    names->setContentsMargins(0, 0, 0, 0);
    names->setSpacing(1);
    identityName_ = new QLabel("No clip selected", identityRow_);
    identityName_->setObjectName("inspector-identity-name");
    identityMeta_ = new QLabel({}, identityRow_);
    identityMeta_->setObjectName("inspector-identity-meta");
    // Both elide rather than wrap: the panel is 300px and a clip name is as
    // long as somebody's camera made it, and a header that grows to two lines
    // moves every control under it every time the selection changes.
    //
    // `Ignored` horizontally is the half that matters. Without it a label asks
    // for the width of its whole string, and the longest name in the project
    // decides how narrow the inspector can be dragged.
    for (QLabel* label : {identityName_, identityMeta_}) {
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        label->installEventFilter(this);
    }
    names->addWidget(identityName_);
    names->addWidget(identityMeta_);
    identity->addLayout(names, 1);
}

void EffectControls::buildInfoGroup() {
    // "Clip" and not "Info": the tab above it already says Info, and a heading
    // that repeats the tab is a line of the panel spent saying nothing.
    infoGroup_ = new QGroupBox("Clip", this);
    infoGroup_->setObjectName("inspector-group-info");
    auto* form = new QFormLayout(infoGroup_);
    infoForm_ = form;
    // Read-only, and labels rather than disabled fields. This page answers
    // questions about the clip; a greyed-out spin box answers them worse than a
    // line of text, and invites a click that does nothing.
    static constexpr const char* kRows[] = {
        "Name", "Track", "Source", "Media", "Sound", "Timeline", "Source range", "Speed", "Angles",
    };
    for (const char* name : kRows) {
        auto* value = new QLabel("\u2014", infoGroup_);
        value->setObjectName("inspector-info-value");
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        form->addRow(QString::fromUtf8(name), value);
        infoValues_.push_back(value);
    }
}

void EffectControls::setPane(Pane pane) {
    pane_ = pane;
    QPushButton* wanted = pane == Pane::Inspector ? inspectorTab_
                          : pane == Pane::Audio   ? audioTab_
                                                  : infoTab_;
    if (wanted != nullptr && !wanted->isChecked()) {
        const QSignalBlocker block{wanted};
        wanted->setChecked(true);
    }
    applyPaneVisibility();
    // Back to the top. The pages are different heights, and arriving at a new
    // one scrolled to where the last one happened to be is how a short page
    // looks empty.
    if (scroll_ != nullptr) {
        scroll_->verticalScrollBar()->setValue(0);
    }
}

void EffectControls::applyPaneVisibility() {
    const bool inspector = pane_ == Pane::Inspector;

    // A track selection is its own page. Everything below describes a clip,
    // and a track has none of it: no transform, no grade, no mask. Handled
    // first and returned from, so none of the clip rules run against a
    // selection that has no clip in it.
    const model::Track* track = selectedTrack();
    trackGroup_->setVisible(track != nullptr && inspector);
    const bool trackSound = track != nullptr && track->kind() == model::TrackKind::Audio;
    trackLevelGroup_->setVisible(trackSound && pane_ == Pane::Audio);
    if (track != nullptr) {
        for (QWidget* widget :
             {enabled_ ? static_cast<QWidget*>(enabled_) : nullptr, videoGroup_, anglesGroup_,
              nestedGroup_, colourGroup_, secondaryGroup_, keyGroup_, effectGroup_, maskGroup_,
              graphicGroup_, textGroup_, audioGroup_, processingGroup_}) {
            if (widget != nullptr) {
                widget->setVisible(false);
            }
        }
        infoGroup_->setVisible(pane_ == Pane::Info);
        inspectorTab_->setEnabled(true);
        // A picture track contributes nothing to the mix, so its Audio tab has
        // nothing behind it -- the same rule a picture clip gets.
        audioTab_->setEnabled(trackSound);
        infoTab_->setEnabled(true);
        // Nothing on this page is a value with a default worth restoring: a
        // name is not "wrong", and un-muting a track is one click away.
        resetButton_->setEnabled(false);
        resetButton_->setToolTip("Nothing to put back on a track");
        return;
    }

    // What the selection is, and what a clip of that sort has to say. One
    // table rather than a condition per group: see `groupsFor`.
    GroupSet groups = kind_ ? groupsFor(*kind_) : GroupSet{};

    // With more than one clip, only what can be written to all of them stays.
    //
    // A mask, a grade chain, an effect stack, a title's words, a multicam
    // clip's cameras: each of those is a piece of work on one clip, and there
    // is no sense in which five clips share one. Showing the primary's while
    // the header says "5 clips" would mean every edit landed on a clip whose
    // name is not the one on screen -- which is the kind of thing somebody
    // discovers an hour later.
    //
    // What survives is what is a *value*: the parameter rows, the blend, the
    // level and pan, the repair. Those five clips can all be set to 80%.
    const bool several = !others_.empty();
    if (several) {
        groups.graphic = false;
        groups.text = false;
        groups.mask = false;
        groups.secondary = false;
        groups.key = false;
        groups.effects = false;
        groups.angles = false;
        groups.nested = false;
        groups.mediaMotion = false;
    }

    videoGroup_->setVisible(inspector && groups.motion);
    colourGroup_->setVisible(inspector && groups.colour);
    secondaryGroup_->setVisible(inspector && groups.secondary);
    keyGroup_->setVisible(inspector && groups.key);
    effectGroup_->setVisible(inspector && groups.effects);
    maskGroup_->setVisible(inspector && groups.mask);
    graphicGroup_->setVisible(inspector && groups.graphic);
    textGroup_->setVisible(inspector && groups.text);
    anglesGroup_->setVisible(inspector && groups.angles);
    nestedGroup_->setVisible(inspector && groups.nested);

    // Inside Colour: the five parameter rows are a value and go to every
    // selected clip, and everything under them is one clip's own work. A curve
    // somebody drew, a LUT they loaded, a set of wheels they balanced -- there
    // is no sense in which three clips share one, and writing the primary's
    // while the header says three would be the silent partial edit this whole
    // rule exists to avoid.
    if (colourForm_ != nullptr) {
        const bool one = others_.empty();
        for (QWidget* widget :
             {static_cast<QWidget*>(curves_), lutRow_, static_cast<QWidget*>(lutName_),
              static_cast<QWidget*>(lutAmount_), wheelsBox_, static_cast<QWidget*>(vignetteAmount_),
              static_cast<QWidget*>(vignetteMidpoint_), static_cast<QWidget*>(vignetteFeather_),
              static_cast<QWidget*>(vignetteRoundness_)}) {
            if (widget != nullptr) {
                colourForm_->setRowVisible(widget, one);
            }
        }
    }
    audioGroup_->setVisible(pane_ == Pane::Audio && groups.audio);
    processingGroup_->setVisible(pane_ == Pane::Audio && groups.audio);

    // Inside Motion: the four rows that place a picture in the frame, and the
    // ones that need a file behind the clip. An adjustment layer has neither
    // and still has an opacity, which is why this is a row-level decision and
    // not another group.
    for (const Row& row : rows_) {
        if (row.form == nullptr || row.line == nullptr || !isPlacementParam(row.param)) {
            continue;
        }
        row.form->setRowVisible(row.line, groups.placement);
    }
    if (motionMediaForm_ != nullptr) {
        for (QWidget* widget : {pinRow_, reframe_ ? static_cast<QWidget*>(reframe_) : nullptr,
                                stabiliseRow_, speedRow_, remapRow_}) {
            if (widget != nullptr) {
                motionMediaForm_->setRowVisible(widget, groups.mediaMotion);
            }
        }
    }

    // A title shares the box and the fill with a shape and has neither a kind
    // to pick nor corners to round, so those rows go and the heading says
    // which of the two this is.
    if (graphicForm_ != nullptr) {
        const bool shape = kind_ == model::ClipKind::Shape;
        for (QWidget* widget :
             {static_cast<QWidget*>(shapeKind_), static_cast<QWidget*>(shapeCorner_),
              static_cast<QWidget*>(shapeFeather_)}) {
            graphicForm_->setRowVisible(widget, shape);
        }
        if (auto* box = qobject_cast<QGroupBox*>(graphicGroup_)) {
            box->setTitle(shape ? "Shape" : "Box");
        }
    }
    infoGroup_->setVisible(pane_ == Pane::Info);
    // Whether the clip plays at all is a fact about the clip rather than about
    // any one page, but on a page of read-only answers it is the only thing
    // that would move, which reads as an oversight rather than as a control.
    //
    // And it needs a clip to be a fact about. Every group hides when nothing
    // is selected, but this box belongs to no group, so it was left behind as
    // a lone dead tick under "No clip selected".
    enabled_->setVisible(pane_ != Pane::Info && selectedClip() != nullptr);

    // A tab with nothing behind it is disabled rather than hidden: the design
    // draws three, and a strip that grew and shrank with the selection would
    // move the one somebody was aiming at.
    inspectorTab_->setEnabled(groups.motion);
    audioTab_->setEnabled(groups.audio);
    infoTab_->setEnabled(selectedClip() != nullptr);
    // Nothing on the Info page to put back.
    resetButton_->setEnabled(pane_ != Pane::Info && selectedClip() != nullptr &&
                             (inspector ? groups.motion : groups.audio));
    // What it puts back depends on what the clip has: an adjustment layer's
    // Motion is an opacity and a blend mode, and promising to restore a
    // position it does not have would be a tooltip describing another panel.
    resetButton_->setToolTip(!inspector         ? "Level to 0 dB and pan to centre"
                             : groups.placement ? "Motion back to its defaults"
                                                : "Opacity and blend back to their defaults");
}

void EffectControls::applyIdentity() {
    if (const model::Track* picked = selectedTrack(); picked != nullptr) {
        const bool sound = picked->kind() == model::TrackKind::Audio;
        identityTile_->setPixmap(icons::pixmap(
            sound ? icons::Glyph::Waveform : icons::Glyph::FilmStrip, 14, theme::accent(300)));
        identityNameFull_ = picked->name().empty()
                                ? QString{sound ? "Sound track" : "Picture track"}
                                : QString::fromStdString(picked->name());
        identityName_->setToolTip(identityNameFull_);

        const std::size_t count = picked->clips().size();
        QString meta = QString("%1 \u00b7 %2 %3")
                           .arg(sound ? "Sound" : "Picture")
                           .arg(count)
                           .arg(count == 1 ? "clip" : "clips");
        if (picked->isLocked()) {
            meta += "  \u00b7  locked";
        }
        identityMetaFull_ = meta;
        elideIdentity();

        // The Info page answers about the track instead. The rows are named
        // for a clip, so the ones that have no track answer are put away
        // rather than filled with a dash.
        const auto dash = QString::fromUtf8("\u2014");
        for (QLabel* value : infoValues_) {
            value->setText(dash);
        }
        if (auto* box = qobject_cast<QGroupBox*>(infoGroup_)) {
            box->setTitle("Track");
        }
        infoValues_[0]->setText(identityNameFull_);
        infoValues_[1]->setText(sound ? "Sound" : "Picture");
        if (infoForm_ != nullptr) {
            // The rows are named for a clip. Two of them mean something else
            // about a track, and a label that lies is worse than a row that is
            // not there.
            if (auto* kindRow = qobject_cast<QLabel*>(infoForm_->labelForField(infoValues_[1]))) {
                kindRow->setText("Kind");
            }
        }
        infoValues_[5]->setText(QString("%1 %2").arg(count).arg(count == 1 ? "clip" : "clips"));
        if (auto* holds = qobject_cast<QLabel*>(
                infoForm_ == nullptr ? nullptr : infoForm_->labelForField(infoValues_[5]))) {
            holds->setText("Holds");
        }
        if (infoForm_ != nullptr) {
            for (std::size_t row = 0; row < infoValues_.size(); ++row) {
                infoForm_->setRowVisible(infoValues_[row], row == 0 || row == 1 || row == 5);
            }
        }
        return;
    }
    if (infoForm_ != nullptr) {
        // Back from a track selection: the rows a clip answers are put back,
        // and `applyIdentity` below hides the ones this clip has no answer for.
        for (QLabel* value : infoValues_) {
            infoForm_->setRowVisible(value, true);
        }
        if (auto* holds = qobject_cast<QLabel*>(infoForm_->labelForField(infoValues_[5]))) {
            holds->setText("Timeline");
        }
        if (auto* kindRow = qobject_cast<QLabel*>(infoForm_->labelForField(infoValues_[1]))) {
            kindRow->setText("Track");
        }
        if (auto* box = qobject_cast<QGroupBox*>(infoGroup_)) {
            box->setTitle("Clip");
        }
    }
    const model::Clip* clip = selectedClip();
    const model::Sequence* sequence =
        project_ == nullptr ? nullptr : project_->findSequence(sequenceId_);
    const model::Track* track = sequence == nullptr ? nullptr : sequence->findTrack(track_);

    if (clip == nullptr) {
        identityNameFull_ = "No clip selected";
        identityMetaFull_.clear();
        identityName_->setToolTip({});
        elideIdentity();
        identityTile_->setPixmap(icons::pixmap(icons::Glyph::FilmStrip, 14, theme::textAt(0.35)));
        for (QLabel* value : infoValues_) {
            value->setText(QString::fromUtf8("\u2014"));
        }
        return;
    }

    const bool isVideo = track != nullptr && track->kind() == model::TrackKind::Video;
    const model::MediaRef* media = project_->findMedia(clip->activeSource());
    const icons::Glyph glyph = clip->graphic.isSet() ? icons::Glyph::Image
                               : isVideo             ? icons::Glyph::FilmStrip
                                                     : icons::Glyph::Waveform;
    identityTile_->setPixmap(icons::pixmap(glyph, 14, theme::accent(300)));

    const QString name = QString::fromStdString(clip->name.empty() ? "Clip" : clip->name);
    identityNameFull_ = name;
    identityName_->setToolTip(name);

    // What the clip is, in one line: how long it runs, and where it sits. The
    // lock lives here too -- it used to be appended to the title, and a reason
    // the controls are dead belongs next to the name of the thing they are dead
    // for.
    const time::RationalTime duration = clip->timelineRange.duration();
    QString meta = QString("%1 \u00b7 %2 frames")
                       .arg(track == nullptr ? "" : QString::fromStdString(track->name()))
                       .arg(static_cast<long long>(duration.frames()));
    if (track != nullptr && track->isLocked()) {
        meta += "  \u00b7  track locked";
    }
    // How many, and that everything shown reaches all of them. Said on the
    // line under the name rather than in place of it, because the name is
    // still the clip whose values are in the boxes -- and somebody has to be
    // able to tell which one that is.
    if (!others_.empty()) {
        meta += QString("  \u00b7  %1 clips selected, editing all")
                    .arg(static_cast<int>(selectionCount()));
    }
    identityMetaFull_ = meta;
    elideIdentity();

    // --- the Info page ----------------------------------------------------
    const auto dash = QString::fromUtf8("\u2014");
    const auto timecode = [&](const time::RationalTime& at, const time::Rational& rate) {
        return QString::fromStdString(
            time::timecodeFromFrames(at.rescaledTo(rate).frames(), rate, false).toString());
    };
    const time::Rational rate =
        sequence != nullptr ? sequence->frameRate() : clip->timelineRange.start().rate();

    infoValues_[0]->setText(name);
    infoValues_[1]->setText(track == nullptr ? dash : QString::fromStdString(track->name()));
    if (media != nullptr) {
        infoValues_[2]->setText(QString::fromStdString(media->path));
    } else if (const model::Sequence* inner =
                   clip->nested.isValid() ? project_->findSequence(clip->nested) : nullptr) {
        // A nested clip has no file, but it does have a source, and naming it
        // is the difference between "there is nothing here" and "it is that
        // sequence".
        infoValues_[2]->setText(QString::fromStdString(inner->name()));
    } else {
        infoValues_[2]->setText(QString{"Generated \u2014 no file"});
    }

    if (media != nullptr && media->info.primaryVideo() != nullptr) {
        const media::VideoStreamInfo& video = *media->info.primaryVideo();
        infoValues_[3]->setText(QString("%1\u00d7%2 at %3 fps")
                                    .arg(video.width)
                                    .arg(video.height)
                                    .arg(QString::fromStdString(video.frameRate.toString())));
    } else {
        infoValues_[3]->setText(dash);
    }
    if (media != nullptr && media->info.primaryAudio() != nullptr) {
        const media::AudioStreamInfo& audio = *media->info.primaryAudio();
        infoValues_[4]->setText(QString("%1 ch at %2 Hz")
                                    .arg(audio.channelCount)
                                    .arg(static_cast<long long>(audio.sampleRate.roundToInt())));
    } else {
        infoValues_[4]->setText(dash);
    }

    infoValues_[5]->setText(QString("%1 \u2013 %2")
                                .arg(timecode(clip->start(), rate))
                                .arg(timecode(clip->endExclusive(), rate)));
    const time::Rational sourceRate = clip->sourceRange.start().rate();
    if (sourceRate.isPositive()) {
        infoValues_[6]->setText(QString("%1 \u2013 %2")
                                    .arg(timecode(clip->sourceRange.start(), sourceRate))
                                    .arg(timecode(clip->sourceRange.endExclusive(), sourceRate)));
    } else {
        infoValues_[6]->setText(dash);
    }

    // Derived rather than stored, because that is what speed is here: the ratio
    // between what a clip reads and how long it takes to read it. See ADR-014.
    const double played = clip->timelineRange.duration().toSecondsDouble();
    const double read = clip->sourceRange.duration().toSecondsDouble();
    infoValues_[7]->setText(played > 0.0 && read > 0.0
                                ? QString("%1%").arg(100.0 * read / played, 0, 'f', 1) +
                                      (clip->reversed ? QString{"  (reversed)"} : QString{})
                                : dash);

    if (clip->isMulticam()) {
        const auto active = static_cast<std::size_t>(
            clip->activeAngle >= 0 &&
                    static_cast<std::size_t>(clip->activeAngle) < clip->angles.size()
                ? clip->activeAngle
                : 0);
        const std::string& live = clip->angles[active].name;
        infoValues_[8]->setText(QString("%1, showing %2")
                                    .arg(clip->angles.size())
                                    .arg(live.empty() ? QString("angle %1").arg(active + 1)
                                                      : QString::fromStdString(live)));
    } else {
        infoValues_[8]->setText(dash);
    }

    // The rows this kind of clip has an answer for. A page of questions where
    // five of eight read "--" is not answering them; it is listing the ones it
    // cannot answer.
    if (infoForm_ != nullptr) {
        const model::ClipKind kind =
            model::clipKindOf(*clip, isVideo ? model::TrackKind::Video : model::TrackKind::Audio);
        const bool file = model::readsMedia(kind);
        // Source stays for a nested clip, where it names the sequence rather
        // than a file, and goes for the generated kinds, which have neither.
        infoForm_->setRowVisible(infoValues_[2], file || kind == model::ClipKind::Nested);
        infoForm_->setRowVisible(infoValues_[3], file);
        infoForm_->setRowVisible(infoValues_[4], file);
        infoForm_->setRowVisible(infoValues_[6], file || kind == model::ClipKind::Nested);
        infoForm_->setRowVisible(infoValues_[7], file || kind == model::ClipKind::Nested);
        infoForm_->setRowVisible(infoValues_[8], kind == model::ClipKind::Multicam);
    }
}

void EffectControls::elideIdentity() {
    // Middle for the name, because the head and the tail of a clip name are the
    // parts that tell two takes apart and the middle is the part they share.
    // End for the line under it, which reads left to right and stops mattering.
    identityName_->QLabel::setText(identityName_->fontMetrics().elidedText(
        identityNameFull_, Qt::ElideMiddle, std::max(0, identityName_->width())));
    identityMeta_->QLabel::setText(identityMeta_->fontMetrics().elidedText(
        identityMetaFull_, Qt::ElideRight, std::max(0, identityMeta_->width())));
}

bool EffectControls::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Resize && (watched == identityName_ || watched == identityMeta_)) {
        elideIdentity();
    }
    // A title's words reach the model when somebody stops typing them, not
    // while they are. Per-keystroke would be one undo step per character and a
    // re-read of the model between each two, which takes the cursor away from
    // wherever it was.
    if (watched == textBody_ && event->type() == QEvent::FocusOut) {
        commitText();
    }
    return QWidget::eventFilter(watched, event);
}

void EffectControls::commitText() {
    if (!textDirty_) {
        return;
    }
    textDirty_ = false;
    pushText();
    // Its own undo step: the next thing typed is a separate edit, and merging
    // them would make one undo take back a paragraph.
    if (commands_ != nullptr) {
        commands_->breakMerge();
    }
}

void EffectControls::resetPane() {
    // Narrower than the tab it sits over, deliberately. On the Inspector page
    // this puts motion back and leaves the grade, the mask and the effect stack
    // alone: those are three separate pieces of work with their own controls,
    // and one arrow that threw all of them away would be the most expensive
    // click in the panel. The tooltip says which it is, and the command stack
    // takes it back either way.
    if (commands_ == nullptr || project_ == nullptr || !clip_.isValid()) {
        return;
    }
    const model::Clip* clip = selectedClip();
    if (clip == nullptr) {
        return;
    }
    // Broken on both sides, so the reset is its own undo step. Parameter edits
    // carry a merge key -- that is what makes dragging a slider one undo rather
    // than one per pixel -- and without this the reset merges into whatever was
    // dragged just before it, so undoing goes back past both and the value the
    // reset replaced is unreachable.
    commands_->breakMerge();
    updating_ = true;
    if (pane_ == Pane::Inspector) {
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
        updating_ = false;
        pushTransform();
    } else if (pane_ == Pane::Audio) {
        gain_->setValue(0.0);
        pan_->setValue(0.0);
        updating_ = false;
        pushAudio();
    }
    updating_ = false;
    commands_->breakMerge();
    refresh();
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
    // Per selected clip, and from what *that* clip already says: a parameter
    // that is animated keeps the static value it had, because the widget is
    // showing the animated one and baking it in would silently change what the
    // picture reverts to when animation is switched off. Which parameters those
    // are differs from clip to clip, so this cannot be worked out once.
    applyToSelection([this](model::TrackId track, model::ClipId id) -> Result<edit::CommandPtr> {
        const model::Clip* each = clipAt(track, id);
        if (each == nullptr) {
            return Error{ErrorCode::NotFound, "no such clip"};
        }
        model::Transform transform = each->transform;
        const auto isAnimated = [each](model::Param param) {
            const model::Curve* curve = each->animation.find(param);
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
        return edit::makeSetTransform(*project_, {sequenceId_, track}, id, transform);
    });
    emit edited();
}

void EffectControls::pushColorCurves(const model::ColorCurves& curves, bool committed) {
    if (updating_ || commands_ == nullptr || !clip_.isValid()) {
        return;
    }
    auto built = edit::makeSetColorCurves(*project_, {sequenceId_, track_}, clip_, curves);
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
    applyToSelection([this, role](model::TrackId track, model::ClipId id) {
        return edit::makeSetAudioRole(*project_, {sequenceId_, track}, id, role);
    });
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
    // Only if the list is actually on something. An index of -1 hands back an
    // invalid QVariant, whose toInt() is 0 -- GraphicKind::None -- so a combo
    // that had nothing selected would unset the graphic and delete the clip's
    // picture on the next edit to any other field.
    const QVariant chosen = shapeKind_->currentData();
    if (chosen.isValid()) {
        graphic.kind = static_cast<model::GraphicKind>(chosen.toInt());
    }
    graphic.width = shapeWidth_->value();
    graphic.height = shapeHeight_->value();
    graphic.cornerRadius = shapeCorner_->value();
    graphic.feather = shapeFeather_->value();
    graphic.red = shapeRed_->value();
    graphic.green = shapeGreen_->value();
    graphic.blue = shapeBlue_->value();
    graphic.alpha = shapeAlpha_->value();

    auto built = edit::makeSetGraphic(*project_, {sequenceId_, track_}, clip_, graphic);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    emit edited();
}

void EffectControls::showAngles() {
    const model::Clip* clip = selectedClip();
    if (clip == nullptr || !clip->isMulticam()) {
        return;
    }
    const auto live = static_cast<std::size_t>(
        clip->activeAngle >= 0 && static_cast<std::size_t>(clip->activeAngle) < clip->angles.size()
            ? clip->activeAngle
            : 0);

    // Rebuilt only when the list is describing a different clip. Refilling it
    // on every read would take the row somebody had picked away from them --
    // the panel re-reads the model whenever the playhead moves.
    const bool rebuild = static_cast<std::size_t>(angleList_->count()) != clip->angles.size();
    const int wanted = rebuild ? static_cast<int>(live) : angleList_->currentRow();

    const QSignalBlocker blockList{angleList_};
    if (rebuild) {
        angleList_->clear();
        for (std::size_t i = 0; i < clip->angles.size(); ++i) {
            angleList_->addItem(QString{});
        }
    }
    for (std::size_t i = 0; i < clip->angles.size(); ++i) {
        const model::Clip::Angle& angle = clip->angles[i];
        const QString name = angle.name.empty() ? QString("Angle %1").arg(i + 1)
                                                : QString::fromStdString(angle.name);
        // The live one is marked in the text rather than by the selection,
        // because the selection is what somebody is *about* to do and the mark
        // is what the clip is already doing. Using one for both would mean
        // looking at a row could not tell you which.
        angleList_->item(static_cast<int>(i))
            ->setText(QString("%1  %2").arg(
                i == live ? QString::fromUtf8("\u25CF") : QString::fromUtf8("\u25CB"), name));
    }
    if (angleList_->currentRow() != wanted) {
        angleList_->setCurrentRow(wanted);
    }
    if (rebuild && angleList_->count() > 0) {
        // Tall enough for the cameras there are, up to six -- measured from a
        // row the list has actually laid out rather than from the font, which
        // is smaller than a row by however much the style puts around it. Six
        // because the panel is a column and the rest of it has to stay
        // reachable; past that the list scrolls.
        const int row = angleList_->sizeHintForRow(0);
        const int rows = std::min(angleList_->count(), 6);
        angleList_->setFixedHeight((row * rows) + (2 * angleList_->frameWidth()) + 4);
    }

    const int row = angleList_->currentRow();
    const bool picked = row >= 0 && static_cast<std::size_t>(row) < clip->angles.size();
    angleOffset_->setEnabled(picked);
    // Switching to the angle that is already live is refused by the operation,
    // so the button says so rather than letting somebody find out.
    angleSwitch_->setEnabled(picked && static_cast<std::size_t>(row) != live &&
                             clip->timelineRange.contains(position_));
    if (picked) {
        const QSignalBlocker blockOffset{angleOffset_};
        angleOffset_->setValue(
            clip->angles[static_cast<std::size_t>(row)].offset.toSecondsDouble());
    }
}

void EffectControls::pushAngleOffset() {
    if (updating_ || commands_ == nullptr || !clip_.isValid()) {
        return;
    }
    const model::Clip* clip = selectedClip();
    const int row = angleList_->currentRow();
    if (clip == nullptr || row < 0 || static_cast<std::size_t>(row) >= clip->angles.size()) {
        return;
    }
    // At the sequence's rate, because that is the timebase the offset is used
    // against when the angle is read.
    const model::Sequence* sequence = project_->findSequence(sequenceId_);
    const time::Rational rate =
        sequence != nullptr ? sequence->frameRate() : clip->timelineRange.start().rate();
    const time::RationalTime offset =
        time::RationalTime::fromSeconds(time::Rational::approximate(angleOffset_->value()), rate);

    auto built = edit::makeSetAngleOffsets(*project_, {sequenceId_, track_}, clip_,
                                           {{static_cast<std::int32_t>(row), offset}});
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    emit edited();
}

void EffectControls::switchToAngle() {
    if (commands_ == nullptr || !clip_.isValid()) {
        return;
    }
    const int row = angleList_->currentRow();
    if (row < 0) {
        return;
    }
    auto built = edit::makeSwitchAngle(*project_, {sequenceId_, track_}, clip_,
                                       static_cast<std::int32_t>(row), position_);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    // The selection keeps the part *before* the cut, which still plays the
    // angle it did -- so the list is right to go on showing that one as live.
    // Switching at the very first frame is not a cut at all and changes the
    // whole clip, which the re-read below picks up either way.
    applyToWidgets();
    emit edited();
}

void EffectControls::pushSpeed() {
    if (updating_ || commands_ == nullptr || !clip_.isValid()) {
        return;
    }
    const model::Clip* clip = selectedClip();
    if (clip == nullptr) {
        return;
    }
    auto built = edit::makeSetSpeed(*project_, {sequenceId_, track_}, clip_,
                                    speed_->value() / 100.0, reverse_->isChecked());
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    // Not re-read into the field. A speed has to land on a whole number of
    // frames, so 33% of a 40 frame clip is 121 frames and reads back as
    // 33.06% -- and writing that into the box a moment after somebody typed
    // 33 would look like the panel arguing with them. The clip is right; the
    // field says what was asked for until the selection changes.
    emit edited();
}

void EffectControls::pushText() {
    if (updating_ || commands_ == nullptr || !clip_.isValid()) {
        return;
    }
    const model::Clip* clip = selectedClip();
    if (clip == nullptr || clip->graphic.kind != model::GraphicKind::Text) {
        return;
    }
    // From what the clip already says, so nothing the graphic group owns --
    // the box, the fill -- is dropped by an edit made here. One `Graphic` is
    // written by two groups, and each has to leave the other's fields alone.
    model::Graphic graphic = clip->graphic;
    graphic.text = textBody_->toPlainText().toStdString();
    graphic.family = textFamily_->currentFont().family().toStdString();
    graphic.pointSize = textSize_->value();
    graphic.bold = textBold_->isChecked();
    graphic.italic = textItalic_->isChecked();
    graphic.alignment = textAlign_->currentData().toInt();

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
    applyToSelection([this](model::TrackId track, model::ClipId id) -> Result<edit::CommandPtr> {
        const model::Clip* each = clipAt(track, id);
        if (each == nullptr) {
            return Error{ErrorCode::NotFound, "no such clip"};
        }
        const auto staticOr = [each](model::Param param, QDoubleSpinBox* spin) {
            const model::Curve* curve = each->animation.find(param);
            return curve != nullptr && !curve->empty() ? each->parameterValue(param)
                                                       : spin->value();
        };
        model::ColorCorrection color;
        color.temperature = staticOr(model::Param::Temperature, temperature_);
        color.tint = staticOr(model::Param::Tint, tint_);
        color.exposure = staticOr(model::Param::Exposure, exposure_);
        color.contrast = staticOr(model::Param::Contrast, contrast_);
        color.saturation = staticOr(model::Param::Saturation, saturation_);
        return edit::makeSetColorCorrection(*project_, {sequenceId_, track}, id, color);
    });
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
    applyToSelection([this](model::TrackId track, model::ClipId id) -> Result<edit::CommandPtr> {
        const model::Clip* each = clipAt(track, id);
        if (each == nullptr) {
            return Error{ErrorCode::NotFound, "no such clip"};
        }
        const auto staticOr = [each](model::Param param, QDoubleSpinBox* spin) {
            const model::Curve* curve = each->animation.find(param);
            return curve != nullptr && !curve->empty() ? each->parameterValue(param)
                                                       : spin->value();
        };
        return edit::makeSetClipAudio(*project_, {sequenceId_, track}, id,
                                      staticOr(model::Param::GainDb, gain_),
                                      staticOr(model::Param::Pan, pan_));
    });
    emit edited();
}

}  // namespace zaro::app
