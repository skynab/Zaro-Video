#pragma once

#include <QWidget>
#include <optional>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/model/Project.h"

namespace zaro::app {

class ProgramMonitor;

/// The handles for editing a mask path, drawn over the picture.
///
/// A transparent widget on top of the monitor rather than something the
/// renderer draws. The renderer's job is the picture that gets delivered, and
/// an outline with handles on it is neither -- putting it in the composite
/// would mean a flag threaded through every path that must never be set during
/// an export.
///
/// It also means the interaction is ordinary Qt: the widget that draws the
/// handles is the widget that receives the clicks, so there is no mapping
/// between two coordinate systems to get wrong beyond the one that matters,
/// which is the letterbox.
class MaskOverlay : public QWidget {
    Q_OBJECT

public:
    explicit MaskOverlay(ProgramMonitor* monitor, QWidget* parent = nullptr);

    /// Nothing is owned. An invalid clip id means there is nothing to edit,
    /// which is also how the overlay is switched off.
    void setTarget(model::Project* project, model::SequenceId sequence, model::TrackId track,
                   model::ClipId clip, edit::CommandStack* commands);

    /// Whether there is a path to edit. The overlay hides itself otherwise, so
    /// it never swallows a click meant for the picture.
    [[nodiscard]] bool isEditing() const;

signals:
    /// The path changed, so anything showing it needs to redraw.
    void edited();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    /// What the mouse is on: a point, or one of the handles either side of it.
    enum class Part : std::uint8_t { Point, HandleIn, HandleOut };
    struct Hit {
        std::size_t index{0};
        Part part{Part::Point};
    };

    [[nodiscard]] const model::Clip* clip() const;
    [[nodiscard]] const model::Mask* mask() const;
    /// Output coordinates -- pixels from the centre of the frame -- to widget
    /// pixels, and back.
    [[nodiscard]] QPointF toWidget(double x, double y) const;
    [[nodiscard]] QPointF toFrame(const QPointF& widget) const;
    [[nodiscard]] std::optional<Hit> hitTest(const QPointF& where) const;
    void apply(const model::MaskPath& path, bool merge);

    ProgramMonitor* monitor_{nullptr};
    model::Project* project_{nullptr};
    model::SequenceId sequenceId_;
    model::TrackId trackId_;
    model::ClipId clipId_;
    edit::CommandStack* commands_{nullptr};
    std::optional<Hit> dragging_;
};

}  // namespace zaro::app
