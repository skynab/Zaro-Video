#pragma once

#include <QPointF>
#include <QRectF>
#include <QWidget>
#include <cstdint>
#include <optional>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/model/Graphic.h"
#include "zaro/core/model/Project.h"

namespace zaro::app {

class ProgramMonitor;

/// The box a title is drawn in, dragged on the picture.
///
/// A title's position and size are numbers in the inspector -- `centreX`,
/// `width` -- and nobody composes a frame by typing numbers into it. This is
/// the same two properties, edited where the answer is visible.
///
/// Built the way `MaskOverlay` is, and for the same reasons: a transparent
/// widget over the monitor, so the renderer never has to know about handles and
/// the widget that draws them is the one that gets the clicks. It asks the
/// monitor where the picture actually is, so it works inside the letterbox.
class TitleOverlay : public QWidget {
    Q_OBJECT

public:
    explicit TitleOverlay(ProgramMonitor* monitor, QWidget* parent = nullptr);

    /// Nothing is owned. An invalid clip id means there is nothing to edit,
    /// which is also how the overlay is switched off.
    void setTarget(model::Project* project, model::SequenceId sequence, model::TrackId track,
                   model::ClipId clip, edit::CommandStack* commands);

    /// Whether the selected clip is a title. The overlay hides itself
    /// otherwise, so it never swallows a click meant for the picture.
    [[nodiscard]] bool isEditing() const;

signals:
    /// The graphic changed, so anything showing it needs to redraw.
    void edited();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    /// What the pointer is on. The corners size the box; anywhere else inside
    /// it moves the box.
    enum class Part : std::uint8_t { None, Body, TopLeft, TopRight, BottomLeft, BottomRight };

    [[nodiscard]] const model::Clip* clip() const;
    [[nodiscard]] const model::Graphic* graphic() const;
    /// Output coordinates -- pixels from the centre of the frame -- to widget
    /// pixels, and back.
    [[nodiscard]] QPointF toWidget(double x, double y) const;
    [[nodiscard]] QPointF toFrame(const QPointF& widget) const;
    /// The graphic's box in widget pixels.
    [[nodiscard]] QRectF boxRect() const;
    [[nodiscard]] Part partAt(const QPointF& where) const;
    /// Write the box back, coalescing: one drag is one undo step, not one per
    /// mouse-move.
    void apply(const model::Graphic& graphic);
    /// Pull an edge to the frame's centre lines and to the title-safe box,
    /// unless the modifier is held. Returns what it latched onto, for the
    /// guide that says so.
    [[nodiscard]] double snapX(double x, bool allow);
    [[nodiscard]] double snapY(double y, bool allow);

    ProgramMonitor* monitor_{nullptr};
    model::Project* project_{nullptr};
    model::SequenceId sequenceId_;
    model::TrackId trackId_;
    model::ClipId clipId_;
    edit::CommandStack* commands_{nullptr};

    Part dragging_{Part::None};
    /// Where the pointer was when the gesture started, and the box it started
    /// from: every move is measured against those rather than against the last
    /// one, so a clamped drag does not accumulate error the pointer never
    /// asked for.
    QPointF grabbedAt_;
    model::Graphic startedFrom_;
    /// What the last move latched onto, in frame coordinates, or nothing.
    std::optional<double> snappedX_;
    std::optional<double> snappedY_;
};

}  // namespace zaro::app
