#pragma once

#include <QWidget>
#include <optional>
#include <vector>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/model/Project.h"

#include "CurveEditor.h"
#include "HueBand.h"

class QCheckBox;
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
    QLabel* lutName_{nullptr};
    QDoubleSpinBox* lutAmount_{nullptr};
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
