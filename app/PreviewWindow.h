// The preview window: the application's main window.
//
// A program monitor over the GPU compositor, with transport. This is the point
// at which the pipeline built in Phase 3 becomes something a person can look
// at: the composited texture goes straight from the compositor to the screen
// without ever being read back into system memory.
//
// Playback here is render-on-demand against the audio clock rather than a
// pre-rendered frame queue. The queue in PlaybackScheduler exists because a
// slow renderer needs somewhere to work ahead; a GPU that composites 1080p in
// under two milliseconds does not, and asking it for the frame the clock is
// currently on is both simpler and lower latency.
//
// It lives in a header rather than inside main.cpp so that the GUI tests in
// app/tests can drive the same window a person drives: a class in an anonymous
// namespace in main.cpp is reachable from nothing.
//
// What is still inline here is what is quicker to read than to look up: the
// accessors, and the one-line forwards to Document, ActionRouter and the
// commands:: operations, where the body is the declaration written twice. The
// constructor and the dialogs are in PreviewWindow.cpp -- they are long, they
// are read one at a time, and they were the reason a change to any panel meant
// recompiling every caller of this header.
#pragma once

#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStringList>
#include <QSysInfo>
#include <QTimer>
#include <QWidget>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "zaro/core/Check.h"
#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/core/edit/Sync.h"
#include "zaro/core/io/CubeLut.h"
#include "zaro/core/io/OtioIo.h"
#include "zaro/core/io/ProjectIo.h"
#include "zaro/core/io/ProjectLock.h"
#include "zaro/core/io/Relink.h"
#include "zaro/core/io/ReviewNotes.h"
#include "zaro/core/io/SubtitleIo.h"
#include "zaro/core/playback/Transport.h"
#include "zaro/core/render/AudioGraph.h"
#include "zaro/core/render/BakeLut.h"
#include "zaro/core/render/ColorPipeline.h"
#include "zaro/core/render/Compare.h"
#include "zaro/core/render/PathRaster.h"
#include "zaro/core/render/Reframe.h"
#include "zaro/core/render/Remix.h"
#include "zaro/core/render/RenderCache.h"
#include "zaro/core/render/RenderGraph.h"
#include "zaro/core/render/SceneDetect.h"
#include "zaro/core/render/Scopes.h"
#include "zaro/core/render/ShotMatch.h"
#include "zaro/core/render/Stabilise.h"
#include "zaro/core/render/ToneMap.h"
#include "zaro/core/render/Tracker.h"
#include "zaro/core/time/Timecode.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"
#include "zaro/platform/qtext/QtTextRasterizer.h"
#include "zaro/platform/sdl/AudioSink.h"
#include "zaro/ui/Actions.h"
#include "zaro/ui/Keymap.h"
#include "zaro/ui/SequenceBinding.h"

#include "ActionRouter.h"
#include "ChannelPanel.h"
#include "ClipStrip.h"
#include "ColorPalette.h"
#include "CurveEditor.h"
#include "DeliverPanel.h"
#include "Document.h"
#include "EffectControls.h"
#include "ExportDialog.h"
#include "FrameThumb.h"
#include "GalleryPanel.h"
#include "GradeNodes.h"
#include "Hotkeys.h"
#include "Icons.h"
#include "LoudnessPanel.h"
#include "MaskOverlay.h"
#include "MediaBrowser.h"
#include "MixerPanel.h"
#include "PlaybackController.h"
#include "ProgramMonitor.h"
#include "ProjectBin.h"
#include "Say.h"
#include "ScopesPanel.h"
#include "SourceMonitor.h"
#include "StemsPanel.h"
#include "SupportButton.h"
#include "Theme.h"
#include "TimelineWidget.h"
#include "Transcript.h"
#include "ViewerOverlay.h"
#include "chrome/Bars.h"
#include "chrome/Choices.h"
#include "chrome/Menus.h"
#include "chrome/Widgets.h"
#include "commands/Analysis.h"
#include "commands/Context.h"
#include "commands/Media.h"
#include "commands/Multicam.h"
#include "commands/Music.h"
#include "commands/Review.h"
#include "commands/Structure.h"
#include "commands/Templates.h"

namespace zaro::app {

// Transport glyphs. Characters rather than an icon font: nothing is vendored
// here, and a glyph that renders as a colour emoji on one platform and an
// empty box on another is worse than a shape every font has.
constexpr const char* kPlayGlyph = "\u25B6";         // BLACK RIGHT-POINTING TRIANGLE
constexpr const char* kPauseGlyph = "\u275A\u275A";  // HEAVY VERTICAL BAR, twice

/// The workspaces, in the order they appear on the tool bar.
inline const QStringList kWorkspaces{"Edit", "Color", "Audio", "Deliver"};

/// What the status line says it is running on.
inline const QString kPlatformLabel = QSysInfo::prettyProductName();

/// Where Donate goes. One constant, because a project's funding page moves and
/// the button should not have to be found again when it does. Until there is a
/// funding page this is the project's own, which is at least somewhere a person
/// who wants to help can start.
inline const QString kSupportUrl = "https://github.com/skynab/Zaro-Video";

// No Q_OBJECT: this declares no signals or slots of its own, and
// QMetaObject::invokeMethod with a lambda needs only a QObject to bind the
// call to. Adding it in a .cpp would also require including its moc output.

class PreviewWindow : public QWidget {
public:
    PreviewWindow(model::Project project, io::LoadedProject loaded, std::string path);

private:
    // Construction, in the order it has to happen: a layout cannot add a
    // widget that does not exist, and a connection cannot name a panel that
    // has not been made. Six phases the constructor already had, now named.
    // See PreviewWindow.cpp.

    /// Every panel the window owns, and the sizes the design fixes them at.
    void createPanels();
    /// Splitters rather than fixed layouts: panel sizes are a matter of what
    /// someone is doing at the time, and the arrangement is remembered
    /// between sessions.
    void buildViewerLayout();
    /// What the bin, the gallery, the strips and the source monitor do to
    /// each other when something in one of them is picked.
    void wireWorkspacePanels();
    /// The timeline pane, the grading palette, the Deliver page, and the
    /// stack that switches between them.
    void buildWindowLayout();
    /// The timeline, the scopes, the mixer, the parameters and the transport.
    void wireEditingSignals();
    /// The meter tick and the autosave, then the first draw.
    void startTimers();

public:
    ~PreviewWindow() override { shutDown(); }

    [[nodiscard]] app::ProgramMonitor* monitor() const { return monitor_; }
    [[nodiscard]] app::TimelineWidget* timeline() const { return timeline_; }
    [[nodiscard]] app::SourceMonitor* sourceMonitor() const { return source_; }
    [[nodiscard]] app::EffectControls* effects() const { return effects_; }
    [[nodiscard]] app::ScopesPanel* scopes() const { return scopes_; }
    [[nodiscard]] app::MixerPanel* mixer() const { return mixer_; }
    [[nodiscard]] app::ChannelPanel* channel() const { return channel_; }
    [[nodiscard]] app::DeliverPanel* deliver() const { return deliver_; }
    [[nodiscard]] render::AudioSource& media() { return *media_; }
    [[nodiscard]] render::SourceFrameProvider* frames() { return media_.get(); }
    [[nodiscard]] render::FrameSource& frameSource() { return *media_; }
    [[nodiscard]] app::ProjectBin* bin() { return bin_; }
    [[nodiscard]] Status reopenMedia() { return openMedia(); }

    /// Re-seat everything that holds a pointer to the active sequence.
    ///
    /// Adding a sequence reallocates the project's vector of them, so anything
    /// holding a pointer into it is looking at freed memory. This window looks
    /// its own up by id; the monitor is handed one, so it has to be told again.
    /// Show a different sequence of the same project.
    ///
    /// The playhead goes back to the start rather than being carried across:
    /// a position means "this far into this sequence", and keeping the number
    /// while changing what it counts into is how a window ends up parked past
    /// the end of something.
    void setActiveSequence(model::SequenceId id);

    /// Point every panel at the sequence being edited.
    ///
    /// One loop over one list. Which panels needed telling used to be written
    /// out by hand in three different places, and between them they named four
    /// of the eleven -- the rest were bound on the way into the workspace that
    /// shows them, so changing sequence while already standing in the mixer
    /// left the console on the sequence that had gone. A list is still a list,
    /// but it is one list, and a panel that does not appear in it cannot be
    /// bound anywhere else either, which is a failure somebody notices.
    void rebindSequence();

    /// What the operations in app/commands need, gathered from where it lives.
    ///
    /// Built per call rather than held: every member of it is something the
    /// window changes, and a cached copy would be a second answer to questions
    /// that already have one.
    [[nodiscard]] commands::Context editContext();

    /// Show what an edit did.
    ///
    /// The five or six lines every operation used to end with. They are the
    /// reason those operations looked like window code: the editing itself
    /// needs none of this, and a caller that forgets one of them gets a picture
    /// that does not match the project rather than an error.
    void afterEdit();

    /// Show what a change to the project's *media* did.
    ///
    /// The bin is what lists the files, so it is the one that has to be told;
    /// the picture may also have changed underneath, since a relinked or
    /// proxied clip decodes from somewhere else now.
    void afterMediaChange();

    /// Take a panel into the list that rebindSequence walks, and bind it now.
    ///
    /// Panels that are made when they are first asked for -- the browser, the
    /// transcript -- arrive after the window is built, so they join here rather
    /// than in the constructor.
    template <typename Panel>
    Panel* adopting(Panel* panel) {
        bound_.push_back(panel);
        panel->bind(ui::SequenceBinding{&document_.project(), sequenceId_, &document_.commands()});
        return panel;
    }

    /// Feed the mixer's meters.
    ///
    /// While playing they come from the thread that is actually producing the
    /// audio, so they show what is being heard. Stopped, a short block is mixed
    /// at the playhead: a mixer whose meters die whenever the transport stops
    /// cannot be used to set a level, which is most of what a mixer is for.
    void updateMeters();

    [[nodiscard]] const model::Sequence* sequence() const { return liveSequence(); }
    [[nodiscard]] model::Project& project() { return document_.project(); }

    /// Block until the background peak generation has finished and its results
    /// have been delivered. For the self-test, which would otherwise capture
    /// the timeline before any waveform arrived.
    void waitForWaveforms();
    /// The active sequence, looked up each time. A handful of sequences and a
    /// linear scan: cheaper than any of the ways of getting this wrong.
    [[nodiscard]] const model::Sequence* liveSequence() const;

    [[nodiscard]] edit::CommandStack& commands() { return document_.commands(); }
    [[nodiscard]] render::RenderCache& renderCache() { return renderCache_; }

    /// Match the selected clip to the frame being held as the reference.
    Result<render::ShotMatch> matchToReference();

    using MaskTrack = commands::MaskTrack;

    /// Follow the selected clip's mask through the rest of the clip.
    Result<commands::MaskTrack> trackMaskForward();

    /// Steady the selected clip.
    Result<render::StabiliseResult> stabiliseClip();

    /// Point the project's media at files that moved.
    Result<io::RelinkReport> relinkMedia(const std::string& root);

    /// Make a small copy of one file to cut against.
    Result<platform::ffmpeg::ProxySummary> buildProxy(model::MediaRefId mediaId,
                                                      std::int32_t width = 960);

    /// Leave a note at the playhead, or take the one that is there away.
    Result<bool> toggleCommentHere();

    /// Write the notes out as a list somebody can be sent.
    Status writeReviewNotes(const std::string& path);

    /// Open the browser, on the folder the project's media came from.
    ///
    /// Starting there rather than at the home folder: somebody browsing for
    /// media in a project that already has some is nearly always looking in
    /// the same place they got the last lot.
    app::MediaBrowser* browseMedia();

    /// Put the keymap's current binding on one action.
    ///
    /// **Only keystrokes with a modifier become Qt shortcuts.** A bare letter
    /// bound through Qt fires while somebody is typing a title into a text
    /// field; those are dispatched from this window's key handler instead,
    /// which only sees a press when nothing else wanted it. That split used to
    /// be a hand-maintained list of "the ones that are not menu items"; it is
    /// derived now, so rebinding Save to "S" moves it to the safe path by
    /// itself.

    /// Re-read every binding: what the manager calls when something changed.
    void applyKeymap() { actions_.applyKeymap(); }

    /// Where a customised keymap lives.
    ///
    /// A file in the user's config folder rather than a value inside the
    /// settings blob: a keymap is a thing people share, back up, put in a
    /// dotfile repository and edit by hand when a shortcut has gone somewhere
    /// they cannot press.

    /// Put the keymap somewhere else.
    ///
    /// For the self-test, which rebinds things and must not leave them rebound
    /// in whoever ran it -- it did exactly that once, and the next run failed
    /// on a Save still sitting where the previous run had moved it. Also how
    /// somebody keeps a keymap beside a project rather than in their home
    /// directory: ZARO_KEYMAP names the file.
    static void setKeymapPath(const QString& path) { ActionRouter::setKeymapPath(path); }

    /// Whether to interrupt. See `app::setQuiet`.
    static void setQuietMode(bool quiet) { app::setQuiet(quiet); }

    void loadKeymap() { actions_.loadKeymap(); }

    void saveKeymap() { actions_.saveKeymap(); }

    /// The hotkey manager.
    app::Hotkeys* showHotkeys();

    /// Gather the project's media into one folder.
    Result<io::ConsolidateReport> consolidateMedia(const std::string& destination);

    /// Show the transcript, and edit by it.
    app::Transcript* showTranscript();

    /// Fit the selected music clip to a length.
    Result<render::RemixPlan> remixSelectedTo(double targetSeconds);

    /// Reframe the selected clip for the sequence's shape.
    Result<render::ReframeResult> reframeClip();

    /// Pin the selected clip to one on a lower track, or to nothing.
    Result<model::ClipId> pinTo(model::ClipId host);

    /// Pin the selected clip to whatever is under it at the playhead.
    Result<model::ClipId> pinToClipBelow();

    /// Save the selected title as a template.
    Status saveGraphicTemplate(const std::string& path);

    /// Drop a saved template in at the playhead.
    Result<model::ClipId> placeGraphicTemplate(const std::string& path,
                                               const time::RationalTime& at);

    /// Hold a frame and show the current one against it.
    void setComparing(bool on, const time::RationalTime& reference);
    [[nodiscard]] bool comparing() const noexcept { return comparing_; }

    void setCompareMode(render::CompareMode mode);
    void setCompareSplit(double split);

    /// What this sequence is delivered as, and how its highlights get there.
    ///
    /// A sequence property rather than an export option, because the curve
    /// editor and the scopes are drawn against it: choosing it at export time
    /// would mean grading against one curve and delivering through another.
    void deliveryMenu();

    /// Set the curve the sequence goes out through.
    bool setDelivery(const model::Sequence::Output& output);

    /// Cut the selected clip where the picture changes.
    std::int32_t detectScenes();

    /// Open a different project into this window.
    ///
    /// Everything is loaded and the media opened *before* anything is replaced,
    /// so a file that cannot be read leaves the window on the project it
    /// already had. Half-swapping a window is how a program ends up showing one
    /// project's timeline over another's media.
    /// Reports rather than reports *and* decides how to tell somebody. The
    /// button below puts the message on screen; a caller with no screen -- the
    /// self-test -- gets the same answer without a dialog it cannot dismiss.
    /// Whether this build takes and honours project locks.
    ///
    /// **Off by default, for now.** The locking is finished and tested, but it
    /// interrupts an ordinary run -- a lock left behind by a killed process, or
    /// one written by a test, produces a dialog somebody has to answer before
    /// they can carry on. Until there is a way to clear a lock from the
    /// interface rather than from the filesystem, the interruption costs more
    /// than the protection is worth. `ZARO_LOCKING=1` turns it back on, and
    /// the self-test turns it on for the block that checks it.
    static bool lockingEnabled() { return Document::lockingEnabled(); }
    static void setLockingEnabled(bool enabled) { Document::setLockingEnabled(enabled); }

    using Sharing = Document::Sharing;

    /// Open a project, deciding first whether anybody else has it.
    [[nodiscard]] Status openProject(const std::string& path, Sharing sharing = Sharing::Exclusive);

    /// Whether this window may write over the project it has open.
    [[nodiscard]] bool isReadOnly() const noexcept { return document_.isReadOnly(); }

    /// Who else has this project, if anybody. Empty when it is ours or free.
    /// Who else has this project, if anybody.
    [[nodiscard]] std::string heldBy() const { return document_.heldBy(); }

    void releaseLock() { document_.releaseLock(); }

    /// Start again, with somewhere to put something.
    void newProject();

    /// Replace what this window is showing.
    ///
    /// The same no-dialog bargain closing makes: whatever was unsaved is
    /// written to its recovery file first, so switching projects never asks a
    /// question and never loses anything.
    /// Replace what this window is showing.
    ///
    /// The document takes the project; everything here is the window catching
    /// up with it -- rebinding the panels, reopening the media, and forgetting
    /// what was selected in a project that has gone.
    void adopt(model::Project project, io::LoadedProject loaded, std::string path,
               bool readOnly = false);

    /// Write the project back where it came from.
    ///
    /// Returns false when it could not be written, so a caller that was about
    /// to do something irreversible knows not to.
    /// Write the project back where it came from.
    ///
    /// Returns false when it could not be written, so a caller that was about
    /// to do something irreversible knows not to.
    bool save();

    /// Save as the next version beside this one, and carry on in it.
    ///
    /// Carrying on in the new file rather than staying in the old one is the
    /// point: a version is a line somebody draws under what they had, and the
    /// next hour's work belongs after the line. The previous file is left
    /// exactly as it was, which is the other half of the point.
    /// Save as the next version beside this one, and carry on in it.
    Result<std::string> saveNewVersion();

    /// The versions beside this project, to jump between.
    void openVersionMenu();

    void openDialog();

    bool saveAs();

    /// Write the recovery file, if there is anything to recover.
    void autosave() { document_.autosave(); }

    [[nodiscard]] const std::string& projectPath() const noexcept { return document_.path(); }

    /// Point Save at a different file. What Save As does once somebody has
    /// chosen one.
    /// Point Save at a different file.
    void setProjectPath(std::string path);

    /// The file name, and whether it differs from what is on disk.
    void updateTitle();

    /// Show the source frame the picture is currently made from.
    void matchFrame();

    /// Make a subclip of what is marked in the source monitor.
    void makeSubclip();

    /// Point the selected clip at a different file.
    void replaceSelectedSource(model::MediaRefId media);

    /// Line the multicam angles up, and say which ones would not.
    void syncAngles(bool byEar);

    [[nodiscard]] std::int32_t lastSyncCount() const noexcept { return lastSyncCount_; }
    [[nodiscard]] std::int32_t lastSyncSkipped() const noexcept { return lastSyncSkipped_; }

    /// Recompute what the cache bar shows.
    ///
    /// Sampled at one point per pixel of the timeline's width: the bar cannot
    /// show more than that, and checking every frame of a long sequence would
    /// hash the whole timeline on every repaint.
    void updateCacheBar();

    /// The work behind the menu entry, separated from the menu so that it can
    /// be driven without one.
    void renderVisibleRange();

    Status openMedia();

    void setPosition(const time::RationalTime& position);

    void step(std::int64_t frames);

protected:
    /// Turn a key press into the same text a keymap holds.
    ///
    /// Qt's own portable spelling, run through the keymap's normaliser, so
    /// there is one form and one comparison rather than a second opinion about
    /// what "Shift+Left" is called.
    [[nodiscard]] static std::string keystrokeOf(const QKeyEvent* event);

    void keyPressEvent(QKeyEvent* event) override;

public:
    /// Run an action by name.
    ///
    /// The single door everything goes through -- a menu item, a key press, a
    /// test -- so "what does this command do" has one answer and a self-test
    /// can press a button that has no button.
    bool trigger(const std::string& actionId) { return actions_.trigger(actionId); }

    [[nodiscard]] const time::RationalTime& position() const noexcept { return position_; }
    [[nodiscard]] const PlaybackController& playback() const noexcept { return playback_; }

    /// Register something that has no menu item: playback, marking, stepping.
    ///
    /// These are the commands whose defaults are bare letters, which cannot be
    /// Qt shortcuts without firing while somebody types. They are actions like
    /// any other -- catalogued, rebindable, listed in the manager -- they
    /// simply arrive through the key handler rather than through a menu.
    /// Register a command that has no menu item: playback, marking, stepping.
    template <typename F>
    void bindAction(const char* actionId, F&& handler) {
        actions_.bind(actionId, std::forward<F>(handler));
    }

    [[nodiscard]] ui::Keymap& keymap() { return actions_.keymap(); }

protected:
    /// Keep the mask handles over the picture when the monitor resizes.
    ///
    /// An event filter rather than a layout: the overlay has to cover the
    /// monitor exactly, and a layout that also managed the monitor's own
    /// children would be a second thing deciding where the picture is.
    bool eventFilter(QObject* watched, QEvent* event) override;

    void closeEvent(QCloseEvent* event) override;

private:
    // --- The chrome ---------------------------------------------------------
    //
    // The window's own furniture: a menu bar, a tool bar, the viewer's header,
    // the transport, the timeline's header and a status line. Built here rather
    // than in a designer file, in the order they are stacked on screen.

    /// Put a command on a menu, by id.
    ///
    /// The QAction comes from the router, which made it when the id was bound;
    /// two menus naming the same id get the same action, and neither has to
    /// know what it does.
    QAction* menuItem(QMenu* menu, const char* actionId);

    /// The commands that arrive by key rather than through a menu.
    void bindPlaybackActions();

    /// Say what every command does, once.
    ///
    /// The menus and the bars name ids and nothing else; this is the only place
    /// that knows what an id means. It used to be spread through buildMenus as a
    /// lambda per menu item, which is why building a menu needed the window:
    /// naming File > Save meant writing out what saving is.
    void bindCommands();

    void buildMenus();

    /// A Window-menu item that shows and hides one panel.
    template <typename F>
    void panelAction(QMenu* menu, const QString& text, F&& panel) {
        QAction* action = menu->addAction(text);
        action->setCheckable(true);
        connect(action, &QAction::triggered, this, [panel](bool on) { panel()->setVisible(on); });
        connect(menu, &QMenu::aboutToShow, this,
                [action, panel] { action->setChecked(panel()->isVisible()); });
    }

    QWidget* buildTitleBar() { return chrome::buildTitleBar(this, bars_); }

    /// The tool palette.
    ///
    /// On the timeline's own header rather than the window's tool bar: every
    /// one of these tools acts on the timeline and nowhere else, and a control
    /// two panels away from the thing it changes is one people stop reaching
    /// for.
    /// The few things the bars do that are not commands. See chrome::Hooks.
    [[nodiscard]] chrome::Hooks chromeHooks();

    QWidget* buildToolBar();

    QWidget* buildViewerBar();

    QWidget* buildTransportBar() { return chrome::buildTransportBar(this, bars_, actions_); }

    QWidget* buildTimelinePane();

    QWidget* buildStatusBar() { return chrome::buildStatusBar(this, bars_); }

    /// Show what is turned on, and say so on the toggles and in the well.
    ///
    /// One place, so the toggles and the monitors cannot disagree: every way in
    /// -- a click on a chip, a clip opened from the bin, a match frame -- ends
    /// up here rather than setting visibility itself.
    void syncViewers();

public:
    /// Turn the source monitor on. Opening a clip from the bin and match frame
    /// both want it up; neither wants the program taken down to get it, which
    /// is what a stack used to make them do.
    void setSourceShown(bool on);
    void setProgramShown(bool on);

    /// Put the program monitor up. Kept under its old name because it is the
    /// same errand -- make sure the cut is on screen -- and it no longer costs
    /// the source to do it.
    void showProgram() { setProgramShown(true); }

    /// Where a workspace's splitter sizes are remembered.
    ///
    /// Versioned, because restoring a size list into a splitter with a
    /// different number of panes leaves the new ones at zero width -- which
    /// looks exactly like a panel that failed to appear. Bump the number when
    /// panes are added or removed from either splitter.
    static QString layoutKey(const QString& workspace, const char* which);

    /// A workspace is which panels are up. Four arrangements, because there are
    /// four things people do with an editor, and each of them wants a different
    /// half of the window: the panels a colourist needs are dead weight while
    /// somebody is assembling, and the reverse.
    void setWorkspace(const QString& name);

private:
    /// Everything in the chrome that describes state rather than causing it.
    /// Gather what the bars say, and hand it to the code that says it.
    ///
    /// The gathering is the window's -- these facts live in eight different
    /// places and only the window knows all of them. The saying is not, and
    /// used to be: fourteen setText calls formatting strings out of a project,
    /// a timeline widget, a bin and a render queue. See chrome::refresh.
    void updateChrome();

    /// Two menu items and a toolbar button that were buttons in a row before.
    void exportDialog();

    void exportOtio();

    void trackMask();

    void stabilise();

    void clearStabilisation();

    void relinkDialog();

    void consolidateDialog();

    void saveTemplateDialog();

    void placeTemplateDialog();

    void exportReviewNotes();

    void matchShot();

    void goToStart();
    void goToEnd();
    void stepBack() { step(-1); }
    void stepForward() { step(1); }

    /// Panel sizes and window geometry, remembered between sessions.
    ///
    /// Saved on close rather than continuously: writing settings on every drag
    /// of a splitter is a lot of disk traffic for something only read once.
    void saveWorkspace();

    void restoreWorkspace();

    /// Stop everything and join. Called from both the close event and the
    /// destructor, because they are not the same path: quitting with Cmd+Q
    /// destroys the window without ever delivering a close event, and a
    /// std::thread destroyed while still joinable calls std::terminate. That
    /// was a real crash -- the application aborted on quit whenever a waveform
    /// scan was still running, which on a freshly opened project is always.
    void shutDown();

    void doNextMarker();
    void doPreviousMarker();

    /// Open the source monitor on the frame the selected clip is showing.
    ///
    /// The mapping is `Clip::activeSourceTimeAt` -- the same one the renderer
    /// asks -- so match frame stays right through trims, speed, reverse, a
    /// multicam angle and a time remap, with nothing to keep in step.
    void doMatchFrame() { matchFrame(); }
    void doDetectScenes() { static_cast<void>(detectScenes()); }
    void doNew() { newProject(); }
    void doOpen() { openDialog(); }
    void doSave() { static_cast<void>(save()); }
    void doSaveAs() { static_cast<void>(saveAs()); }
    void doMarkIn() { source_->markIn(); }
    void doMarkOut() { source_->markOut(); }
    void doInsert() { placeFromSource(edit::PlaceMode::Insert); }
    void doOverwrite() { placeFromSource(edit::PlaceMode::Overwrite); }
    void doSourceBack() { source_->step(-1); }
    void doSourceForward() { source_->step(1); }

    /// Three-point edit: the marked range from the source, at the playhead.
    void placeFromSource(edit::PlaceMode mode);

    void togglePlay() { playback_.togglePlay(position_); }

    void startIfPlaying() { playback_.startIfPlaying(position_); }

    void stop() { playback_.stop(); }

    /// Peaks are generated off the UI thread: decoding a long file's audio
    /// takes seconds, and a project that freezes while it opens is worse than
    /// one whose waveforms arrive a moment late.
    void startWaveforms();

    /// Measure the frame at the playhead, if anyone is looking at the scopes.
    ///
    /// Not during playback, and not when the panel is hidden. Measuring means
    /// compositing a frame on the CPU, and doing that on every frame of
    /// playback would spend more time on the instrument than on the picture --
    /// the readback the GPU path exists to avoid, reintroduced through the
    /// back door.
    ///
    /// It composites through render::RenderGraph rather than reading back the
    /// preview, so the scope measures what will be delivered. That is the
    /// number a grade is judged against, and it does not make playback pay for
    /// a readback it otherwise does not need.
    /// Composite the frame under the playhead, and give it to whatever wants it.
    ///
    /// One render for both instruments. The scopes measure it and the Audio
    /// workspace's thumbnail shows it, and they are never both up -- but
    /// compositing twice for the one that is would be paying for the frame
    /// twice, and the frame is the expensive part.
    ///
    /// Not while playing. Both of these are things somebody looks at when
    /// stopped, and a composite per displayed frame is the transport's budget
    /// spent on a picture that is already on screen.
    void refreshInstruments();

    /// The composited frame, encoded for display and handed to the thumbnail.
    void showThumbnail(const render::RgbaImage& frame);

    /// Measure the whole programme's loudness.
    ///
    /// On demand, not continuously: the reading that matters is the gated
    /// integrated figure over everything, and that means mixing the sequence
    /// from end to end. Doing it every time a fader moved would make the mixer
    /// unusable to pay for a number nobody reads until delivery.
    void measureProgramme();

    /// Say which stages of the chain this shot has been through.
    ///
    /// Read from the clip rather than remembered, for the same reason the
    /// panels are: after an undo the clip is the only thing that knows.
    void refreshGradeChain();

    /// Keep the frame that is on screen, as a reference to grade against.
    ///
    /// Grabbed off the monitor rather than composited again: what somebody
    /// means by "this frame" is the one they are looking at, and rendering a
    /// second one would be a different picture the moment anything about the
    /// grade or the proxy setting differed.
    void grabStill();

    /// Put a .cube on the selected shot.
    void applyLookToSelection(const QString& path);

    /// Attach proxies, make them, and switch between them and the originals.
    void proxyMenu();

    /// Work out the offsets between a multicam clip's angles.
    ///
    /// Two methods, because a shoot is either jam-synced or it is not, and
    /// there is no useful middle: timecode is exact when it is there, and by
    /// ear is what is left when it is not.
    void multicamMenu();

    /// Pre-render, so a stack the CPU has to composite plays back.
    ///
    /// The visible range rather than the whole sequence: what somebody wants
    /// rendered is what they are about to watch, and "render everything" on a
    /// long timeline is a decision to wait for frames nobody asked about. The
    /// range is chosen by scrolling and zooming, which they are doing anyway.
    void renderMenu();

    /// Measure the programme, and offer to normalise it.
    ///
    /// The measurement is of the whole sequence through the real mix, so it
    /// takes a moment on a long one — which is why it is a menu action rather
    /// than a meter that runs continuously. Loudness is a delivery check, done
    /// once near the end, not something to watch while cutting.
    void loudnessMenu();

    /// Import, export and burn-in, from one menu.
    ///
    /// A menu rather than a panel: captions are imported once, exported once,
    /// and otherwise left alone, and a permanent panel for three actions would
    /// take room from the ones used constantly.
    void captionsMenu();

    void applyCaptions(const model::CaptionTrack& captions);

    void refresh();

    /// The project, its history, its path and its lock.
    Document document_;
    /// The transport, the audio clock and the thread that feeds it.
    PlaybackController playback_{this};
    QTimer* autosaveTimer_{nullptr};
    /// The active sequence, by id rather than by pointer.
    ///
    /// A pointer into the project's vector of sequences is valid until one is
    /// added, and adding one is now an ordinary thing to do -- making a
    /// sequence to nest is how nesting starts. The dangling pointer presented
    /// as a zero denominator deep inside rational arithmetic, which is a long
    /// way from the cause.
    model::SequenceId sequenceId_;
    std::unique_ptr<platform::ffmpeg::ProjectMediaSource> media_;
    /// One font engine for the window: the preview, the scopes and the export
    /// dialog all draw the same titles.
    platform::qtext::QtTextRasterizer text_;
    /// Composited frames, shared between the preview's CPU fallback and the
    /// pre-render. One cache: a frame rendered by the button is the frame the
    /// monitor asks for a moment later, and two caches would render it twice.
    render::RenderCache renderCache_;
    /// Comparison view: a way of looking, not part of the cut, so none of this
    /// is saved with the project.
    bool comparing_{false};
    time::RationalTime referenceAt_{};
    render::CompareMode compareMode_{render::CompareMode::Split};
    double compareSplit_{0.5};
    /// What the timeline last said was picked. Multicam syncing acts on one
    /// clip, and this is which one.
    model::TrackId selectedTrack_;
    model::ClipId selectedClip_;
    std::int32_t lastSyncCount_{0};
    std::int32_t lastSyncSkipped_{0};

    app::ProgramMonitor* monitor_{nullptr};
    app::MaskOverlay* maskOverlay_{nullptr};
    app::TimelineWidget* timeline_{nullptr};
    app::EffectControls* effects_{nullptr};
    app::ScopesPanel* scopes_{nullptr};
    app::MixerPanel* mixer_{nullptr};
    QTimer* meterTimer_{nullptr};
    app::ProjectBin* bin_{nullptr};
    app::GalleryPanel* gallery_{nullptr};
    app::LoudnessPanel* loudness_{nullptr};
    app::FrameThumb* thumb_{nullptr};
    app::StemsPanel* stems_{nullptr};
    app::ChannelPanel* channel_{nullptr};
    app::ClipStrip* clipStrip_{nullptr};
    app::GradeNodes* nodes_{nullptr};
    app::ColorPalette* palette_{nullptr};
    /// Every panel that is about a sequence, in one place. See rebindSequence.
    std::vector<ui::SequenceBound*> bound_;
    app::MediaBrowser* browser_{nullptr};
    app::Transcript* transcript_{nullptr};
    app::Hotkeys* hotkeys_{nullptr};

    /// Which keystroke runs what, and everything that can be run.
    ActionRouter actions_{this};
    app::SourceMonitor* source_{nullptr};
    QSplitter* topSplitter_{nullptr};
    QSplitter* mainSplitter_{nullptr};

    /// The chrome. None of it owns anything: every one of these is a child of
    /// the window, and Qt deletes them with it.
    chrome::Bars bars_;
    bool sourceShown_{false};
    bool programShown_{true};
    app::DeliverPanel* deliver_{nullptr};
    app::ViewerOverlay* viewerOverlay_{nullptr};
    QString workspace_{"Edit"};

    time::RationalTime position_{};

    std::thread waveformThread_;
    std::atomic<bool> shuttingDown_{false};
};

}  // namespace zaro::app
