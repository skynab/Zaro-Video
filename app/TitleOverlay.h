#pragma once

#include <QPointF>
#include <QRectF>
#include <QWidget>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/model/Graphic.h"
#include "zaro/core/model/Project.h"

class QPlainTextEdit;

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

    /// Whether the text is being typed into, here on the picture.
    ///
    /// Separate from `isEditing`, which is about the box: a title is always
    /// draggable while it is selected, and only sometimes being typed into.
    [[nodiscard]] bool isTyping() const;

    /// Open the text for typing, or put it away keeping what was typed.
    ///
    /// Public because a double-click is not the only way in or out: picking a
    /// different clip has to close the editor, or it would be left sitting over
    /// somebody else's title writing into a clip that is no longer selected.
    void beginTyping();
    void endTyping();

signals:
    /// The graphic changed, so anything showing it needs to redraw.
    void edited();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    /// The monitor letterboxes as the window changes shape, so the box moves
    /// and an open editor has to move with it.
    void resizeEvent(QResizeEvent* event) override;
    /// Watches the editor for the keys that finish or abandon a typing pass,
    /// and for it losing focus.
    bool eventFilter(QObject* watched, QEvent* event) override;

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
    /// Put the editor over the box, in something close to the face the title
    /// is actually drawn in, so what is typed sits where it will end up.
    void layOutEditor();
    /// Give the text back the way it was when typing started, and close.
    void abandonTyping();

    ProgramMonitor* monitor_{nullptr};
    model::Project* project_{nullptr};
    model::SequenceId sequenceId_;
    model::TrackId trackId_;
    model::ClipId clipId_;
    edit::CommandStack* commands_{nullptr};

    Part dragging_{Part::None};
    /// What a press landed on, while it is still only a press.
    ///
    /// A drag does not begin until the pointer has gone far enough to mean one.
    /// Without that, the press inside a double-click started a gesture, the
    /// hand's own tremor between the two clicks was a move, and snapping pulled
    /// the box onto the nearest guide -- so opening a title to type in it moved
    /// the title.
    Part pending_{Part::None};
    /// Where the pointer was when the gesture started, and the box it started
    /// from: every move is measured against those rather than against the last
    /// one, so a clamped drag does not accumulate error the pointer never
    /// asked for.
    QPointF grabbedAt_;
    model::Graphic startedFrom_;
    /// What the last move latched onto, in frame coordinates, or nothing.
    std::optional<double> snappedX_;
    std::optional<double> snappedY_;

    /// Made on the first double-click and kept, because a widget rebuilt each
    /// time is a widget whose focus and geometry have to be re-established each
    /// time. Null until then; a child, so the overlay owns it.
    QPlainTextEdit* editor_{nullptr};
    /// Where the history stood when the button went down.
    ///
    /// A double-click arrives as a press, a release and then the double-click,
    /// and the press is a press like any other: it may already have dragged the
    /// box before the second click says what the gesture actually was. This is
    /// what that gets unwound to.
    std::size_t stepsAtPress_{0};
    /// Where the history stood when typing started. Escape unwinds back to it,
    /// which is how abandoning a pass leaves no step behind rather than leaving
    /// a step that changes nothing.
    std::size_t stepsBefore_{0};
    /// Set while the editor is being filled in from the model, so the change it
    /// emits is not written straight back as if somebody had typed it.
    bool fillingEditor_{false};
    /// Whether a typing pass is open.
    ///
    /// Held rather than read back off the editor's visibility: a widget is not
    /// visible while any ancestor is hidden, so asking Qt would say the pass had
    /// ended every time the window was minimised -- and then a press would drag
    /// the box somebody had a caret in, and the pass would never be closed off.
    bool typing_{false};
};

}  // namespace zaro::app
