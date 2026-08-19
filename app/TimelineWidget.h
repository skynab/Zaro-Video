#pragma once

#include <QWidget>
#include <map>
#include <memory>
#include <optional>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/media/Waveform.h"
#include "zaro/core/model/Project.h"
#include "zaro/core/time/RationalTime.h"
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
    explicit TimelineWidget(QWidget* parent = nullptr);

    /// Neither is owned; both must outlive the widget.
    void setProject(model::Project* project, model::SequenceId sequence,
                    edit::CommandStack* commands);

    [[nodiscard]] const time::RationalTime& playhead() const noexcept { return playhead_; }
    void setPlayhead(const time::RationalTime& position);

    void zoomToFit();

    /// Peaks for a media reference, drawn on its audio clips. Shared because
    /// generation happens on another thread and several clips may cite the
    /// same source.
    void setWaveform(model::MediaRefId media, std::shared_ptr<const media::Waveform> waveform);

    /// Read-only view of the geometry, so a caller can work out where on screen
    /// a given time lands. Used by the edit self-test to aim at a clip edge.
    [[nodiscard]] const ui::TimelineLayout& layout() const noexcept { return layout_; }

    /// Row geometry for a track, for the same reason.
    [[nodiscard]] std::optional<ui::TimelineLayout::Row> rowFor(model::TrackId track) const;

signals:
    void playheadMoved(const zaro::time::RationalTime& position);
    /// The model changed, so anything showing it needs to repaint.
    void edited();
    /// What is selected now. An invalid clip id means nothing is.
    void selectionChanged(zaro::model::TrackId track, zaro::model::ClipId clip);

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
    void paintTracks(QPainter& painter);
    void paintClips(QPainter& painter, const ui::TimelineLayout::Row& row);
    void paintWaveform(QPainter& painter, const model::Clip& clip, const QRectF& body);
    void paintTransitions(QPainter& painter, const ui::TimelineLayout::Row& row);
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

    void razorAtPlayhead();
    void addDissolveAtPlayhead();
    void removeSelected(bool ripple);

    model::Project* project_{nullptr};
    model::SequenceId sequenceId_;
    edit::CommandStack* commands_{nullptr};

    ui::TimelineLayout layout_;
    time::RationalTime playhead_{};
    model::ClipId selected_;
    model::TrackId selectedTrack_;

    enum class Drag { None, Scrub, MoveClip, TrimIn, TrimOut };
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
    bool snapEnabled_{true};
    std::map<std::uint64_t, std::shared_ptr<const media::Waveform>> waveforms_;
    /// Fit once the widget knows how wide it really is.
    bool pendingFit_{false};
};

}  // namespace zaro::app
