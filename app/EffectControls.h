#pragma once

#include <QWidget>
#include <array>
#include <optional>
#include <vector>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/model/Project.h"
#include "zaro/core/render/FrameSource.h"

#include "CurveEditor.h"
#include "HueBand.h"

class QCheckBox;
class QListWidget;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QPushButton;
class QToolButton;
class QLabel;

namespace zaro::app {

/// Effect Controls: the parameters of whichever clip is selected.
///
/// Every change goes through the command stack with a merge key, so dragging a
/// value is one undo step rather than one per pixel, and nothing here can move
/// a clip or collide with a neighbour.
///
/// The panel is driven *from* the model rather than holding its own copy: after
/// any edit, and after undo, it re-reads what the clip actually says. A panel
/// that trusted its own widgets would drift out of step the first time
/// something changed the clip from elsewhere.
class EffectControls : public QWidget {
    Q_OBJECT

public:
    explicit EffectControls(QWidget* parent = nullptr);

    void setProject(model::Project* project, model::SequenceId sequence,
                    edit::CommandStack* commands);

    /// Show this clip's parameters. An invalid id clears the panel.
    void setSelection(model::TrackId track, model::ClipId clip);

    /// Show whether the pen is out. Called back when the overlay turns it off
    /// by itself -- the path closed, or was abandoned -- so the button and the
    /// picture never disagree about what a click will do.
    void setDrawingMask(bool drawing);

    /// Where the dialogue is read from when ducking. Not owned.
    void setAudioSource(render::AudioSource* audio) { audio_ = audio; }

    /// Where the playhead is.
    ///
    /// An animated parameter has no single value, so the panel shows the value
    /// at this moment and writes keyframes here. Without it the panel would
    /// have to invent a time, and every keyframe would land in the same place.
    void setPosition(const time::RationalTime& position);

    /// Re-read the model, after an undo or an edit made elsewhere.
    void refresh();

signals:
    /// A parameter changed, so anything showing the picture needs repainting.
    void edited();

    /// Keyframes were added, removed or moved, so the timeline's keyframe lane
    /// needs redrawing even though no picture changed.
    void keyframesChanged();

    /// The pen was turned on or off. The panel does not own the overlay that
    /// draws on the picture, so it asks rather than reaching for it.
    void drawMaskRequested(bool drawing);

    /// Pin this clip to whatever is under it, or let it go. The panel knows
    /// the clip but not what is beneath it at the playhead, which is a
    /// question about the whole sequence.
    void pinRequested();
    void unpinRequested();

    /// Follow the mask through the rest of the clip. The panel has the mask
    /// but not the pictures, so this is asked for rather than done here.
    void trackMaskRequested();

    /// Recompose this clip for the sequence's shape.
    void reframeRequested();

    /// Hold this clip still, or throw the analysis away. The panel has neither
    /// the frames nor the decoder, so it asks.
    void stabiliseRequested();
    void clearStabilisationRequested();

private:
    /// One animatable parameter: its spin box, its stopwatch, and the button
    /// that adds or removes a keyframe at the playhead.
    struct Row {
        model::Param param{};
        QDoubleSpinBox* spin{nullptr};
        QToolButton* stopwatch{nullptr};
        QToolButton* keyframe{nullptr};
    };

    [[nodiscard]] const model::Clip* selectedClip() const;
    /// The playhead in the selected clip's source time, or nothing if the
    /// playhead is not over the clip. A keyframe outside the clip's own range
    /// is unreachable and undeletable from the panel, so the buttons that would
    /// create one are disabled there instead.
    [[nodiscard]] std::optional<time::RationalTime> keyframeTime() const;
    void addRow(QFormLayout* form, const QString& label, model::Param param, QDoubleSpinBox* spin);
    void pushParameter(model::Param param, double value);
    void toggleAnimated(model::Param param, bool on);
    void toggleKeyframe(model::Param param);
    void applyKeyframeButtons();
    void pushTransform();
    void pushAudio();
    void pushColor();
    void pushCurves(const model::ToneCurves& curves, bool committed);
    void pushSecondary();
    void pushLut(const model::LutRef& lut);
    void pushGraphic();
    void pushMask();
    /// The secondary as the widgets currently describe it.
    [[nodiscard]] model::Secondary secondaryFromWidgets() const;
    void applyToWidgets();
    void setEditingEnabled(bool enabled);

    model::Project* project_{nullptr};
    model::SequenceId sequenceId_;
    edit::CommandStack* commands_{nullptr};
    model::TrackId track_;
    model::ClipId clip_;
    time::RationalTime position_;
    std::vector<Row> rows_;

    /// True while the panel is writing values into its own widgets, so their
    /// change signals do not bounce straight back into the model.
    bool updating_{false};

    QLabel* title_{nullptr};
    QDoubleSpinBox* positionX_{nullptr};
    QDoubleSpinBox* positionY_{nullptr};
    QDoubleSpinBox* scaleX_{nullptr};
    QDoubleSpinBox* scaleY_{nullptr};
    QDoubleSpinBox* rotation_{nullptr};
    QDoubleSpinBox* anchorX_{nullptr};
    QDoubleSpinBox* anchorY_{nullptr};
    QDoubleSpinBox* opacity_{nullptr};
    QComboBox* blend_{nullptr};
    QDoubleSpinBox* gain_{nullptr};
    QDoubleSpinBox* pan_{nullptr};
    QDoubleSpinBox* temperature_{nullptr};
    QDoubleSpinBox* tint_{nullptr};
    QDoubleSpinBox* exposure_{nullptr};
    QDoubleSpinBox* contrast_{nullptr};
    QDoubleSpinBox* saturation_{nullptr};
    QCheckBox* enabled_{nullptr};
    QWidget* videoGroup_{nullptr};
    QWidget* colorGroup_{nullptr};
    CurveEditor* curves_{nullptr};
    QWidget* maskGroup_{nullptr};
    QComboBox* maskShape_{nullptr};
    QDoubleSpinBox* maskWidth_{nullptr};
    QDoubleSpinBox* maskHeight_{nullptr};
    QDoubleSpinBox* maskX_{nullptr};
    QDoubleSpinBox* maskY_{nullptr};
    QDoubleSpinBox* maskCorner_{nullptr};
    QDoubleSpinBox* maskFeather_{nullptr};
    QCheckBox* maskInverted_{nullptr};
    QPushButton* maskToPath_{nullptr};
    QPushButton* maskDraw_{nullptr};
    QPushButton* maskTrack_{nullptr};
    QDoubleSpinBox* introSeconds_{nullptr};
    QDoubleSpinBox* outroSeconds_{nullptr};
    void pushResponsive();
    QPushButton* pin_{nullptr};
    QPushButton* unpin_{nullptr};
    QPushButton* reframe_{nullptr};
    QPushButton* stabilise_{nullptr};
    QPushButton* unstabilise_{nullptr};
    void convertMaskToPath();
    QWidget* graphicGroup_{nullptr};
    QComboBox* shapeKind_{nullptr};
    QDoubleSpinBox* shapeWidth_{nullptr};
    QDoubleSpinBox* shapeHeight_{nullptr};
    QDoubleSpinBox* shapeCorner_{nullptr};
    QDoubleSpinBox* shapeFeather_{nullptr};
    QDoubleSpinBox* shapeRed_{nullptr};
    QDoubleSpinBox* shapeGreen_{nullptr};
    QDoubleSpinBox* shapeBlue_{nullptr};
    QWidget* secondaryGroup_{nullptr};
    QPushButton* lutLoad_{nullptr};
    QPushButton* lutClear_{nullptr};
    QPushButton* lutSave_{nullptr};
    void saveLookAsCube();
    QLabel* lutName_{nullptr};
    QDoubleSpinBox* lutAmount_{nullptr};
    /// Time remapping: a switch rather than a stopwatch, because a remap that
    /// is not keyframed is not a remap -- see model::Param::TimeRemap.
    QCheckBox* timeRemap_{nullptr};
    QPushButton* freeze_{nullptr};

    /// The effect stack. A fixed pool of parameter rows, relabelled from
    /// model::parametersOf, so that adding an effect to the model is data
    /// rather than another widget here.
    static constexpr int kMaxEffectParams = 4;
    QListWidget* effectList_{nullptr};
    QComboBox* effectKind_{nullptr};
    QPushButton* effectAdd_{nullptr};
    QPushButton* effectRemove_{nullptr};
    QPushButton* effectUp_{nullptr};
    QPushButton* effectDown_{nullptr};
    QCheckBox* effectEnabled_{nullptr};
    std::array<QLabel*, kMaxEffectParams> effectParamLabels_{};
    std::array<QDoubleSpinBox*, kMaxEffectParams> effectParamSpins_{};
    std::array<QToolButton*, kMaxEffectParams> effectParamStopwatches_{};
    std::array<QToolButton*, kMaxEffectParams> effectParamKeyframes_{};
    /// Which parameter each row is showing at the moment. The rows are a pool
    /// that gets relabelled, so a handler cannot capture its parameter the way
    /// the fixed rows above do.
    std::array<model::EffectParam, kMaxEffectParams> effectParamOf_{};
    void toggleEffectAnimated(int row, bool on);
    void toggleEffectKeyframe(int row);
    QWidget* effectGroup_{nullptr};
    void pushEffects();
    void showEffects();
    bool applyEffectStack(const std::vector<model::Effect>& stack);

    QComboBox* role_{nullptr};
    QPushButton* duck_{nullptr};
    render::AudioSource* audio_{nullptr};
    void pushRole();
    void duckUnderDialogue();

    /// Shadows, midtones and highlights: offset, power and slope per channel.
    std::array<QDoubleSpinBox*, 9> wheels_{};
    void pushWheels();

    QDoubleSpinBox* vignetteAmount_{nullptr};
    QDoubleSpinBox* vignetteMidpoint_{nullptr};
    QDoubleSpinBox* vignetteFeather_{nullptr};
    QDoubleSpinBox* vignetteRoundness_{nullptr};
    void pushVignette();

    QComboBox* keyKind_{nullptr};
    QDoubleSpinBox* keyRed_{nullptr};
    QDoubleSpinBox* keyGreen_{nullptr};
    QDoubleSpinBox* keyBlue_{nullptr};
    QDoubleSpinBox* keyTolerance_{nullptr};
    QDoubleSpinBox* keySoftness_{nullptr};
    QDoubleSpinBox* keySpill_{nullptr};
    QDoubleSpinBox* keyLumaLow_{nullptr};
    QDoubleSpinBox* keyLumaHigh_{nullptr};
    QCheckBox* keyShowMatte_{nullptr};
    QWidget* keyGroup_{nullptr};
    void pushKeyer();

    QCheckBox* qualifierOn_{nullptr};
    QCheckBox* showMask_{nullptr};
    HueBand* hueBand_{nullptr};
    QDoubleSpinBox* hueCentre_{nullptr};
    QDoubleSpinBox* hueWidth_{nullptr};
    QDoubleSpinBox* hueSoftness_{nullptr};
    QDoubleSpinBox* satLow_{nullptr};
    QDoubleSpinBox* satHigh_{nullptr};
    QDoubleSpinBox* lumaLow_{nullptr};
    QDoubleSpinBox* lumaHigh_{nullptr};
    QDoubleSpinBox* keyExposure_{nullptr};
    QDoubleSpinBox* keySaturation_{nullptr};
    QDoubleSpinBox* keyTemperature_{nullptr};
    QWidget* audioGroup_{nullptr};
};

}  // namespace zaro::app
