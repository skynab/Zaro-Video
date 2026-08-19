#pragma once

#include <QWidget>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/model/Project.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
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

    /// Re-read the model, after an undo or an edit made elsewhere.
    void refresh();

signals:
    /// A parameter changed, so anything showing the picture needs repainting.
    void edited();

private:
    [[nodiscard]] const model::Clip* selectedClip() const;
    void pushTransform();
    void pushAudio();
    void applyToWidgets();
    void setEditingEnabled(bool enabled);

    model::Project* project_{nullptr};
    model::SequenceId sequenceId_;
    edit::CommandStack* commands_{nullptr};
    model::TrackId track_;
    model::ClipId clip_;

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
    QCheckBox* enabled_{nullptr};
    QWidget* videoGroup_{nullptr};
    QWidget* audioGroup_{nullptr};
};

}  // namespace zaro::app
