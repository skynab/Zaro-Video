#pragma once

#include <QPoint>
#include <QRect>
#include <QWidget>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/core/media/Waveform.h"
#include "zaro/core/model/Project.h"
#include "zaro/core/time/RationalTime.h"
#include "zaro/core/time/TimeRange.h"
#include "zaro/ui/TimelineLayout.h"

namespace zaro::app {

/// The timeline panel.
///
/// All geometry lives in ui::TimelineLayout, which has no toolkit in it and is
/// unit-tested; this is painting and input on top. Painting is culled to the
/// visible range, so a four-hour sequence costs the same to draw as a
/// four-minute one.
///
/// Every edit goes through the command stack, which is the only write path into
/// the model — so everything here is undoable by construction rather than by
/// remembering to make it so.
class TimelineWidget : public QWidget {
    Q_OBJECT

public:
    /// What a press on a clip means.
    ///
    /// Tools rather than modifiers for the gestures that have no obvious
    /// chord: cutting, slipping and panning are each somebody's whole session,
    /// and a tool that stays picked is what makes a run of them fluent.
    /// Trimming and moving keep working under Select as well, because those
    /// two are what a pointer is for.
    enum class Tool { Select, Blade, Trim, Slip, Hand, Zoom };

    explicit TimelineWidget(QWidget* parent = nullptr);

    [[nodiscard]] Tool tool() const noexcept { return tool_; }
    void setTool(Tool tool);

    /// Whether edges pull to the edit points near them.
    [[nodiscard]] bool snapEnabled() const noexcept { return snapEnabled_; }
    void setSnapEnabled(bool enabled);

    /// Neither is owned; both must outlive the widget.
    void setProject(model::Project* project, model::SequenceId sequence,
                    edit::CommandStack* commands);

    [[nodiscard]] const time::RationalTime& playhead() const noexcept { return playhead_; }
    void setPlayhead(const time::RationalTime& position);

    void zoomToFit();
    /// Zoom about the middle of the view, which is what a button press means:
    /// there is no pointer position to anchor on.
    void zoomBy(double factor);
    /// The current zoom as a fraction of the range the zoom control offers,
    /// and the way back. A slider needs a position, and pixels-per-second is
    /// logarithmic in what it feels like.
    [[nodiscard]] double zoomFraction() const;
    void setZoomFraction(double fraction);

    /// The edits the menus reach for. Each is the same path the keyboard takes,
    /// so a menu item and a key press are the same action rather than two
    /// implementations of it.
    void undo();
    void redo();
    void razorAtPlayhead();
    void addDissolveAtPlayhead();
    void addMarkerAtPlayhead();
    void selectAll();

    /// Peaks for a media reference, drawn on its audio clips. Shared because
    /// generation happens on another thread and several clips may cite the
    /// same source.
    void setWaveform(model::MediaRefId media, std::shared_ptr<const media::Waveform> waveform);

    /// Read-only view of the geometry, so a caller can work out where on screen
    /// a given time lands. Used by the edit self-test to aim at a clip edge.
    [[nodiscard]] const ui::TimelineLayout& layout() const noexcept { return layout_; }

    /// The stretches of the timeline that are pre-rendered, drawn as a bar
    /// along the bottom of the ruler.
    ///
    /// Pushed in rather than queried: working out what is cached costs a hash
    /// per sampled frame, and a repaint happens on every scrub. The owner
    /// recomputes when something could have changed it; the bar only draws.
    void setCachedSpans(std::vector<time::TimeRange> spans);
    [[nodiscard]] const std::vector<time::TimeRange>& cachedSpans() const noexcept {
        return cachedSpans_;
    }

    /// Row geometry for a track, for the same reason.
    [[nodiscard]] std::optional<ui::TimelineLayout::Row> rowFor(model::TrackId track) const;

signals:
    void playheadMoved(const zaro::time::RationalTime& position);
    /// The model changed, so anything showing it needs to repaint.
    void edited();
    /// What is selected now. An invalid clip id means nothing is.
    void selectionChanged(zaro::model::TrackId track, zaro::model::ClipId clip);
    /// The tool or the snap setting changed, including from the keyboard --
    /// so the toolbar showing them can follow rather than only lead.
    void toolChanged();
    void snapChanged(bool enabled);
    /// The visible span of time changed -- scrolled, zoomed or resized.
    /// Anything that summarises the *view* rather than the model, such as the
    /// cache bar, is recomputed from here rather than on every repaint.
    void viewChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    [[nodiscard]] const model::Sequence* sequence() const;
    void paintRuler(QPainter& painter);
    void paintCacheBar(QPainter& painter);
    std::vector<time::TimeRange> cachedSpans_;
    void paintTracks(QPainter& painter);
    void paintClips(QPainter& painter, const ui::TimelineLayout::Row& row);
    void paintKeyframes(QPainter& painter, const model::Clip& clip, const QRectF& body);
    void dragKeyframeTo(int x);
    void paintWaveform(QPainter& painter, const model::Clip& clip, const QRectF& body);
    void paintTransitions(QPainter& painter, const ui::TimelineLayout::Row& row);
    void paintMarkers(QPainter& painter);
    void paintPlayhead(QPainter& painter);

    /// Keep the playhead on screen, paging when it leaves.
    void followPlayhead();

    void scrubTo(int x);
    void beginDrag(const ui::TimelineLayout::Hit& hit, int x, bool ripple);
    void updateDrag(int x);
    void updateTrim(int x);
    void finishDrag();

    /// Pull a time to the nearest edit point, unless the modifier is held.
    [[nodiscard]] time::RationalTime maybeSnap(const time::RationalTime& t,
                                               model::ClipId ignoring) const;

    /// Cut a named track at a named time, which is what the blade does: the
    /// playhead is not involved, and neither is the selection.
    void razorAt(model::TrackId track, const time::RationalTime& at);
    /// Press handling for the tool that is picked, before the Select-tool
    /// paths run. Returns whether the tool consumed the press.
    bool pressWithTool(const ui::TimelineLayout::Hit* hit, int x, int y,
                       Qt::KeyboardModifiers modifiers);
    void updateSlip(int x);
    void updatePan(int x);
    /// The cursor this tool wants over this point.
    void applyCursor(const ui::TimelineLayout::Hit* hit);
    void removeSelected(bool ripple);
    void switchAngle(int angle);

public:
    /// Select one clip, for a self-test that needs a selection without a
    /// mouse. The keyboard paths all act on the selection, so a test that
    /// cannot make one cannot reach them.
    void selectOnlyForTest(model::TrackId track, model::ClipId clip);

    /// Change the transition under the playhead to another kind.
    ///
    /// Separate from adding one, because that is how it is used: somebody drops
    /// a dissolve on a cut and then decides it wants to be a wipe.
    bool setTransitionKindAtPlayhead(model::TransitionKind kind,
                                     model::TransitionDirection direction);

private:
    model::Project* project_{nullptr};
    model::SequenceId sequenceId_;
    edit::CommandStack* commands_{nullptr};

    ui::TimelineLayout layout_;
    time::RationalTime playhead_{};
    /// Everything selected. The first entry is the primary selection, which is
    /// what the Effect Controls panel shows: a panel of parameters has to be
    /// about one clip, even when several are selected for moving.
    std::vector<edit::ClipRef> selection_;
    model::ClipId selected_;
    model::TrackId selectedTrack_;
    /// The selected clip's link group, so its partners can be outlined too.
    model::LinkId selectedLink_;

    [[nodiscard]] bool isSelected(model::ClipId clip) const;
    void selectOnly(const ui::TimelineLayout::Hit& hit);
    void toggleSelected(const ui::TimelineLayout::Hit& hit);
    void announceSelection();
    void removeSelection(bool ripple);

    enum class Drag {
        None,
        Scrub,
        MoveClip,
        TrimIn,
        TrimOut,
        Band,
        MaybeBand,
        Keyframe,
        Slip,
        Pan
    };

    /// The keyframe being dragged, or an invalid clip id when none is.
    ///
    /// Held by (clip, time) rather than by pointer: an edit rebuilds the
    /// clips -- undo restores a whole snapshot -- and a pointer into them would
    /// be dangling by the second mouse-move of a drag.
    struct KeyframeDrag {
        model::TrackId track;
        model::ClipId clip;
        time::RationalTime time;
    };
    KeyframeDrag keyframeDrag_;
    Drag drag_{Drag::None};
    /// Where in the clip the drag started, so it does not jump to the pointer.
    time::RationalTime grabOffset_{};
    /// Where the edge being trimmed currently sits.
    ///
    /// Trims are expressed as deltas, so each mouse move has to be measured
    /// against where the edge actually ended up rather than where it started:
    /// re-reading it after every step keeps a clamped trim from accumulating
    /// error the pointer never asked for.
    time::RationalTime trimAnchor_{};
    bool rippleTrim_{false};
    /// Where a press landed, so a click and a drag can be told apart.
    QPoint pressAt_;
    QRect band_;
    Tool tool_{Tool::Select};
    /// Where a slip was last applied from, so each mouse move asks for the
    /// difference rather than the whole gesture again.
    int slipAnchorX_{0};
    /// Where a pan started, in pixels and in time.
    int panAnchorX_{0};
    time::RationalTime panAnchorScroll_{};
    bool snapEnabled_{true};
    std::map<std::uint64_t, std::shared_ptr<const media::Waveform>> waveforms_;
    /// Fit once the widget knows how wide it really is.
    bool pendingFit_{false};
};

}  // namespace zaro::app
