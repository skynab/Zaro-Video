#pragma once

#include <QFont>
#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/core/edit/Snapping.h"
#include "zaro/core/media/Waveform.h"
#include "zaro/core/model/Project.h"
#include "zaro/core/time/RationalTime.h"
#include "zaro/core/time/TimeRange.h"
#include "zaro/ui/SequenceBinding.h"
#include "zaro/ui/TimelineLayout.h"

#include "MediaDrag.h"

class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QLineEdit;

namespace zaro::app {

class ThumbnailCache;

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
class TimelineWidget : public QWidget, public ui::SequenceBound {
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
    void bind(const ui::SequenceBinding& binding) override;

    [[nodiscard]] const time::RationalTime& playhead() const noexcept { return playhead_; }
    void setPlayhead(const time::RationalTime& position);

    /// How tall the rows of one kind are drawn.
    ///
    /// Per kind rather than per track, which is how the layout stores it and
    /// how the question is usually asked: sound wants room when you are
    /// working on sound, and the picture tracks can shrink while you do it.
    ///
    /// Clamped, and clamped again against the panel: the tracks do not scroll
    /// vertically, so a height that pushed a track off the bottom would make it
    /// unreachable rather than roomy. Growing redistributes the space there is.
    void setTrackHeight(model::TrackKind kind, int pixels);
    [[nodiscard]] int trackHeight(model::TrackKind kind) const;
    /// The heights the panel opens with, for the double-click that resets one.
    static constexpr int kDefaultVideoTrackHeight = 62;
    static constexpr int kDefaultAudioTrackHeight = 48;

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

    /// Where video clips get their filmstrip frames. Not owned; may be null,
    /// in which case clips are drawn as plain blocks. Set by the window, which
    /// is also what shares one cache between the panels that want frames.
    void setThumbnailCache(ThumbnailCache* cache);

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
    /// The whole selection, primary first.
    ///
    /// Alongside the one above rather than replacing it: most of what listens
    /// wants the clip somebody picked -- the mask overlay has one target, the
    /// grade chain describes one clip -- and only the parameter panel has
    /// anything to say about the rest.
    void selectionSetChanged(const std::vector<zaro::edit::ClipRef>& clips);
    /// A track was picked, by pressing its header away from the buttons on it.
    /// An invalid id means a clip was picked instead, and the track selection
    /// is off. The two are exclusive: a panel showing a track's properties and
    /// a clip's at once would have two things called "the selection".
    void trackSelected(zaro::model::TrackId track);
    /// The tool or the snap setting changed, including from the keyboard --
    /// so the toolbar showing them can follow rather than only lead.
    void toolChanged();
    void snapChanged(bool enabled);
    /// The visible span of time changed -- scrolled, zoomed or resized.
    /// Anything that summarises the *view* rather than the model, such as the
    /// cache bar, is recomputed from here rather than on every repaint.
    void viewChanged();
    /// Cut the selected clip where the picture changes.
    ///
    /// A request rather than the edit itself. The analysis decodes every frame
    /// of the clip, which wants a progress dialog and a way to cancel, and the
    /// window owns both -- so the timeline says what was asked for and leaves
    /// the doing to whoever is listening.
    void detectScenesRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    /// Escape in the rename editor abandons the edit. It has to be caught here
    /// rather than in keyPressEvent: while the editor has focus, the key events
    /// are its, not ours.
    bool eventFilter(QObject* watched, QEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    // A file dragged out of the media pane. The bin says which file; where it
    // is let go of says which track it joins and when it starts.
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    [[nodiscard]] const model::Sequence* sequence() const;
    void paintRuler(QPainter& painter);
    void paintCacheBar(QPainter& painter);
    std::vector<time::TimeRange> cachedSpans_;
    void paintTracks(QPainter& painter);
    void paintClips(QPainter& painter, const ui::TimelineLayout::Row& row);
    void paintKeyframes(QPainter& painter, const model::Clip& clip, const QRectF& body);
    void dragKeyframeTo(int x);
    /// `colour` is the clip family's own, so an audio clip's envelope is drawn
    /// in the same teal as the strip above it.
    void paintWaveform(QPainter& painter, const model::Clip& clip, const QRectF& body,
                       const QColor& colour);
    /// Frames across the body of a video clip.
    ///
    /// Returns whether anything was drawn, because the clip's name has to be
    /// legible over a picture and that costs a scrim it does not otherwise
    /// need.
    bool paintFilmstrip(QPainter& painter, const model::Clip& clip, const QRectF& body);
    /// Throw away filmstrip decodes queued for a zoom level that has changed.
    void discardQueuedThumbnails();
    void paintTransitions(QPainter& painter, const ui::TimelineLayout::Row& row);
    /// The eye/speaker, the lock and the remove button in a track's header.
    void paintTrackControls(QPainter& painter, const ui::TimelineLayout::Row& row,
                            const model::Track& track);
    /// The box above the headers, alongside the ruler: where tracks are added.
    void paintHeaderCorner(QPainter& painter);
    void paintMarkers(QPainter& painter);
    void paintPlayhead(QPainter& painter);
    /// The alignment guide: where the current gesture latched, drawn across
    /// every track so the alignment it made is visible on the tracks it was
    /// made against.
    void paintSnapGuide(QPainter& painter);
    /// Where the blade would cut, while it is only hovering.
    void paintBladePreview(QPainter& painter);
    /// Where a file being dragged over the panel would land.
    void paintDropPreview(QPainter& painter);

    /// What a drop at a given point would do.
    ///
    /// Worked out on every mouse-move so the panel can draw it, and worked out
    /// again on the drop rather than trusted: conforming an empty sequence to
    /// the first file changes the frame rate the start time is expressed in.
    struct DropSpot {
        /// One clip's destination.
        struct Landing {
            model::TrackKind kind{model::TrackKind::Video};
            /// The track it joins. Invalid when a new one is wanted, which is
            /// what happens when the clip would land on top of something.
            model::TrackId track;
            bool newTrack{false};
        };
        Landing at;
        /// Where its sound goes.
        ///
        /// A file with both streams arrives as two clips, linked -- the
        /// arrangement `zaro-cut` writes and the one the mixer can hear, since
        /// the audio graph reads clips on audio tracks and nothing else. Empty
        /// when the file is silent, and when the drop was aimed at a sound row
        /// in the first place: that gesture means "use the sound of this take",
        /// and it has already landed where it was asked to.
        std::optional<Landing> sound;
        time::RationalTime start{};
        time::RationalTime duration{};
    };
    [[nodiscard]] std::optional<DropSpot> dropSpotFor(const MediaDrag& dragged, const QPoint& at);
    /// Which track of its kind a clip of this length would land on at this
    /// time: the one asked for, or a new one when that is taken or locked.
    [[nodiscard]] DropSpot::Landing landingFor(model::TrackKind kind, model::TrackId wanted,
                                               const time::TimeRange& range) const;
    /// Where the ghost for a landing is drawn: the clip's own body on an
    /// existing row, or the strip a new row would occupy.
    [[nodiscard]] QRectF dropPreviewRect(const DropSpot& spot,
                                         const DropSpot::Landing& landing) const;
    /// Put one clip down, making its track first if it needs one. Returns the
    /// track it ended up on, invalid if it could not be placed.
    [[nodiscard]] model::TrackId placeOne(const MediaDrag& dragged, const DropSpot& where,
                                          const DropSpot::Landing& landing, model::ClipId& placed);
    /// Make the edit a drop asks for: a track first if it needs one, then the
    /// clip, both inside one undo step.
    void placeDropped(const MediaDrag& dragged, const DropSpot& where);

    /// Keep the playhead on screen, paging when it leaves.
    void followPlayhead();

    void scrubTo(int x);
    void beginDrag(const ui::TimelineLayout::Hit& hit, int x, bool ripple);
    /// Follow the pointer with the clip being dragged.
    ///
    /// `y` as well as `x`: a clip may be moved to another row of its own kind,
    /// which is the only thing the vertical half of the gesture can mean.
    void updateDrag(int x, int y);
    void updateTrim(int x);
    void finishDrag();

    /// Pull a time to the nearest edit point, unless the modifier is held.
    ///
    /// Not const: it remembers what the time latched onto, so the gesture that
    /// asked can show it. A snap nobody can see is indistinguishable from a
    /// hand that happened to be steady.
    ///
    /// `includePlayhead` is false when the playhead itself is what is moving:
    /// it is a candidate at zero distance from where it already is, so leaving
    /// it in makes a scrub of less than the snap radius do nothing at all.
    [[nodiscard]] time::RationalTime maybeSnap(const time::RationalTime& t, model::ClipId ignoring,
                                               bool includePlayhead = true);

    /// The controls a track header carries, and the ones in the corner above
    /// them. Hit-tested here rather than in TimelineLayout: they are buttons on
    /// a widget, and their geometry is a fact about how this panel is drawn
    /// rather than about how time maps to pixels.
    enum class HeaderControl { None, Mute, Lock, Remove };
    enum class CornerControl { None, AddVideo, AddAudio };

    struct HeaderHit {
        model::TrackId track;
        HeaderControl control{HeaderControl::None};
    };

    /// The mono face the V1/A2 badge is set in, and the badge itself.
    [[nodiscard]] QFont badgeFont() const;
    [[nodiscard]] static QString trackBadge(const ui::TimelineLayout::Row& row);
    /// Where a track's name is drawn, and where the editor sits to change it.
    /// One function so the two cannot disagree about it.
    [[nodiscard]] QRect trackNameRect(const ui::TimelineLayout::Row& row,
                                      const model::Track& track) const;

    /// Edit a track's name in place.
    ///
    /// In the header rather than in a dialog: naming tracks is something done
    /// in a run, at the start of a cut, and a modal that has to be dismissed
    /// between each one turns six names into six interruptions.
    void beginRenameTrack(model::TrackId track);
    /// Everything a track header can do, on its own right-click.
    ///
    /// The same four actions the header already carries, named. The three
    /// buttons are quicker once you know which is which; this is where you find
    /// out, and it is the only place Rename is written down.
    void trackHeaderMenu(model::TrackId track, const QPoint& at);
    /// What a clip can do, on its own right-click.
    ///
    /// Selects what was right-clicked first. A menu that acted on the previous
    /// selection while the pointer sat over a different clip would be acting on
    /// something other than the thing being pointed at.
    void clipMenu(const ui::TimelineLayout::Hit& hit, const QPoint& at);
    /// Commit what was typed, or abandon it. Both end the edit.
    void finishRename(bool keep);

    [[nodiscard]] QRect headerControlRect(const ui::TimelineLayout::Row& row,
                                          HeaderControl control) const;
    [[nodiscard]] QRect cornerControlRect(CornerControl control) const;
    [[nodiscard]] HeaderHit headerHitTest(int x, int y) const;
    [[nodiscard]] CornerControl cornerHitTest(int x, int y) const;
    /// Whichever header control the pointer is over, so it can light up. A
    /// button that does not react to being pointed at does not look like one.
    void updateHeaderHover(int x, int y);

    /// The kind whose height the boundary under this point would resize, if the
    /// point is on one.
    ///
    /// A boundary belongs to the row above it. Every row's bottom edge moves
    /// down as its own kind grows -- video stacks upward from the audio block,
    /// audio downward from it -- so "drag the edge down to make this taller"
    /// holds for all of them without exception.
    [[nodiscard]] std::optional<model::TrackKind> headerResizeAt(int x, int y) const;
    void updateTrackHeight(int y);
    /// Put the asked-for heights into the layout, squashed to whatever the
    /// panel can actually show.
    void applyTrackHeights();

    /// A track's own settings, each one command on the stack.
    ///
    /// `muted` is what an eye means on a video track and what a speaker means
    /// on an audio one -- the model keeps one flag, because "do not include
    /// this track" is one idea whichever sense it is about.
    void toggleTrackMute(model::TrackId track);
    void toggleTrackLock(model::TrackId track);
    void removeTrack(model::TrackId track);
    void addTrack(model::TrackKind kind);

    /// Cut a named track at a named time, which is what the blade does: the
    /// playhead is not involved, and neither is the selection.
    void razorAt(model::TrackId track, const time::RationalTime& at);
    /// Press handling for the tool that is picked, before the Select-tool
    /// paths run. Returns whether the tool consumed the press.
    bool pressWithTool(const ui::TimelineLayout::Hit* hit, int x, int y,
                       Qt::KeyboardModifiers modifiers);
    void updateSlip(int x);
    void updatePan(int x);
    /// Follow the pointer with the blade, so where a cut would land -- and what
    /// it would line up with -- is visible before the click rather than after.
    void updateBladeHover(int x, int y);
    void clearGestureMarks();
    /// The cursor this tool wants over this point.
    void applyCursor(const ui::TimelineLayout::Hit* hit);
    void removeSelected(bool ripple);
    void switchAngle(int angle);

public:
    /// Whether an alignment guide is up, and at what time.
    ///
    /// What the guide is painted from, so a test can ask whether the gesture
    /// it just made actually latched onto anything -- the alternative is
    /// counting dashed pixels, which tests the dash pattern as much as the
    /// snap.
    [[nodiscard]] bool showingSnapGuide() const noexcept { return snapMark_.active; }
    [[nodiscard]] const time::RationalTime& snapGuideTime() const noexcept {
        return snapMark_.time;
    }

    /// Select one clip, without a mouse.
    ///
    /// Two callers, and they want the same thing: the Color workspace's shot
    /// strip, where picking a tile is picking a clip, and a self-test that has
    /// to make a selection before it can reach the keyboard paths.
    void selectOnly(model::TrackId track, model::ClipId clip);

    /// Add a clip to the selection without making it the primary, the way
    /// shift-clicking one does.
    ///
    /// The mouse-free twin of `selectOnly`, and wanted for the same reason: a
    /// self-test that has to build a selection of several before it can check
    /// what the parameter panel does with one. Selecting a clip that is already
    /// in the set leaves it where it is rather than promoting it -- shift-click
    /// does not reorder, and the primary is what the panel's header names.
    void selectAlso(model::TrackId track, model::ClipId clip);

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
    /// The track whose header was pressed, if the selection is a track rather
    /// than a set of clips.
    model::TrackId headSelected_;
    model::ClipId selected_;
    model::TrackId selectedTrack_;
    /// The selected clip's link group, so its partners can be outlined too.
    model::LinkId selectedLink_;

    [[nodiscard]] bool isSelected(model::ClipId clip) const;
    void selectOnly(const ui::TimelineLayout::Hit& hit);
    /// Move an already-selected clip to the front of the set, so it is the one
    /// a drag acts on. The set is unchanged; only which of them leads it.
    void makePrimary(const ui::TimelineLayout::Hit& hit);
    void toggleSelected(const ui::TimelineLayout::Hit& hit);
    void announceSelection();
    void removeSelection(bool ripple);

    enum class Drag {
        None,
        Scrub,
        TrackHeight,
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

    /// What the last snapped time latched onto. `SnapKind` and the track come
    /// straight from `edit::snapTime`, which returns them for exactly this.
    struct SnapMark {
        bool active{false};
        time::RationalTime time{};
        edit::SnapKind kind{edit::SnapKind::None};
        model::TrackId track;
    };
    SnapMark snapMark_;

    /// Where the blade is pointing, while it is pointing at something.
    struct BladeMark {
        bool active{false};
        model::TrackId track;
        time::RationalTime time{};
    };
    BladeMark bladeMark_;
    /// Where the file currently being dragged over the panel would land, and
    /// what it carries. Empty whenever nothing is hovering.
    std::optional<DropSpot> dropSpot_;
    MediaDrag dragged_;
    /// The track whose name is being edited, and the editor doing it. The
    /// editor is a child widget, so it is destroyed with this one.
    model::TrackId renamingTrack_;
    QLineEdit* renameEditor_{nullptr};
    /// The kind being resized, where the drag started, and the height it
    /// started from -- so each move measures the whole gesture rather than
    /// accumulating steps that a clamp would make lossy.
    /// The heights asked for, which is not always what is on screen -- see
    /// applyTrackHeights.
    int wantedVideoHeight_{kDefaultVideoTrackHeight};
    int wantedAudioHeight_{kDefaultAudioTrackHeight};
    std::optional<model::TrackKind> resizeKind_;
    int resizeAnchorY_{0};
    int resizeStartHeight_{0};
    HeaderHit hoverHeader_;
    CornerControl hoverCorner_{CornerControl::None};
    std::map<std::uint64_t, std::shared_ptr<const media::Waveform>> waveforms_;
    ThumbnailCache* thumbnails_{nullptr};
    /// Fit once the widget knows how wide it really is.
    bool pendingFit_{false};
};

}  // namespace zaro::app
