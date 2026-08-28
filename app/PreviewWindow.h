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
    void setActiveSequence(model::SequenceId id) {
        if (document_.project().findSequence(id) == nullptr) {
            return;
        }
        document_.project().setActiveSequence(id);
        sequenceId_ = id;
        position_ = time::RationalTime{0, document_.project().findSequence(id)->frameRate()};
        selectedTrack_ = model::TrackId{};
        selectedClip_ = model::ClipId{};
        renderCache_.clear();
        rebindSequence();
        refresh();
    }

    /// Point every panel at the sequence being edited.
    ///
    /// One loop over one list. Which panels needed telling used to be written
    /// out by hand in three different places, and between them they named four
    /// of the eleven -- the rest were bound on the way into the workspace that
    /// shows them, so changing sequence while already standing in the mixer
    /// left the console on the sequence that had gone. A list is still a list,
    /// but it is one list, and a panel that does not appear in it cannot be
    /// bound anywhere else either, which is a failure somebody notices.
    void rebindSequence() {
        const ui::SequenceBinding binding{&document_.project(), sequenceId_, &document_.commands()};
        for (ui::SequenceBound* panel : bound_) {
            if (panel != nullptr) {
                panel->bind(binding);
            }
        }
        monitor_->setSource(liveSequence(), media_.get());
        playback_.setSource(liveSequence(), media_.get());
        monitor_->setNesting(&document_.project(), media_.get());
        monitor_->setRenderCache(&renderCache_);
        monitor_->setTextRasterizer(&text_);
        monitor_->update();
    }

    /// What the operations in app/commands need, gathered from where it lives.
    ///
    /// Built per call rather than held: every member of it is something the
    /// window changes, and a cached copy would be a second answer to questions
    /// that already have one.
    [[nodiscard]] commands::Context editContext() {
        return commands::Context{
            ui::SequenceBinding{&document_.project(), sequenceId_, &document_.commands()},
            selectedTrack_,
            selectedClip_,
            position_,
            media_.get(),
            &renderCache_,
            &text_};
    }

    /// Show what an edit did.
    ///
    /// The five or six lines every operation used to end with. They are the
    /// reason those operations looked like window code: the editing itself
    /// needs none of this, and a caller that forgets one of them gets a picture
    /// that does not match the project rather than an error.
    void afterEdit() {
        document_.commands().breakMerge();
        renderCache_.clear();
        monitor_->update();
        timeline_->update();
        effects_->refresh();
        updateCacheBar();
        updateTitle();
    }

    /// Show what a change to the project's *media* did.
    ///
    /// The bin is what lists the files, so it is the one that has to be told;
    /// the picture may also have changed underneath, since a relinked or
    /// proxied clip decodes from somewhere else now.
    void afterMediaChange() {
        // Reopened here rather than by the operation that changed the paths:
        // the source belongs to the window, and an operation that reached in to
        // reopen it was an operation that could only be called from one.
        if (Status reopened = openMedia(); !reopened) {
            app::warn(this, "Media", QString::fromStdString(reopened.error().toString()));
        }
        renderCache_.clear();
        monitor_->update();
        timeline_->update();
        bin_->refresh();
        updateCacheBar();
        updateTitle();
    }

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
    void updateMeters() {
        if (mixer_ == nullptr || liveSequence() == nullptr || !mixer_->isVisible()) {
            return;
        }
        if (playback_.isPlaying()) {
            mixer_->setMeters(playback_.meters());
            return;
        }
        if (media_ == nullptr) {
            return;
        }
        render::AudioGraph meterMix{*media_};
        const auto& audioRate = liveSequence()->audioSampleRate();
        constexpr std::int64_t kBlock = 1024;
        if (meterMix.mix(*liveSequence(), position_.rescaledTo(audioRate), kBlock, 2)) {
            mixer_->setMeters(meterMix.meters());
        }
    }

    [[nodiscard]] const model::Sequence* sequence() const { return liveSequence(); }
    [[nodiscard]] model::Project& project() { return document_.project(); }

    /// Block until the background peak generation has finished and its results
    /// have been delivered. For the self-test, which would otherwise capture
    /// the timeline before any waveform arrived.
    void waitForWaveforms() {
        if (waveformThread_.joinable()) {
            waveformThread_.join();
        }
        // The results are handed over through the event loop, so they are not
        // on screen until it has been given a chance to run.
        QApplication::processEvents();
    }
    /// The active sequence, looked up each time. A handful of sequences and a
    /// linear scan: cheaper than any of the ways of getting this wrong.
    [[nodiscard]] const model::Sequence* liveSequence() const {
        return document_.project().findSequence(sequenceId_);
    }

    [[nodiscard]] edit::CommandStack& commands() { return document_.commands(); }
    [[nodiscard]] render::RenderCache& renderCache() { return renderCache_; }

    /// Match the selected clip to the frame being held as the reference.
    Result<render::ShotMatch> matchToReference() {
        if (!comparing_) {
            return Error{ErrorCode::InvalidData, "hold a frame to match against first"};
        }
        auto matched = commands::matchToReference(editContext(), referenceAt_);
        if (matched && matched->usable) {
            afterEdit();
        }
        return matched;
    }

    using MaskTrack = commands::MaskTrack;

    /// Follow the selected clip's mask through the rest of the clip.
    Result<commands::MaskTrack> trackMaskForward() {
        auto tracked = commands::trackMaskForward(editContext());
        if (tracked) {
            afterEdit();
        }
        return tracked;
    }

    /// Steady the selected clip.
    Result<render::StabiliseResult> stabiliseClip() {
        auto steadied = commands::stabiliseClip(editContext());
        if (steadied) {
            afterEdit();
        }
        return steadied;
    }

    /// Point the project's media at files that moved.
    Result<io::RelinkReport> relinkMedia(const std::string& root) {
        auto report = commands::relinkMedia(editContext(), root);
        if (report) {
            afterMediaChange();
        }
        return report;
    }

    /// Make a small copy of one file to cut against.
    Result<platform::ffmpeg::ProxySummary> buildProxy(model::MediaRefId mediaId,
                                                      std::int32_t width = 960) {
        auto built = commands::buildProxy(editContext(), mediaId, width);
        if (built) {
            afterMediaChange();
        }
        return built;
    }

    /// Leave a note at the playhead, or take the one that is there away.
    Result<bool> toggleCommentHere() {
        auto toggled = commands::toggleCommentHere(editContext());
        if (toggled) {
            timeline_->update();
            updateTitle();
        }
        return toggled;
    }

    /// Write the notes out as a list somebody can be sent.
    Status writeReviewNotes(const std::string& path) {
        return commands::writeReviewNotes(editContext(), path);
    }

    /// Open the browser, on the folder the project's media came from.
    ///
    /// Starting there rather than at the home folder: somebody browsing for
    /// media in a project that already has some is nearly always looking in
    /// the same place they got the last lot.
    app::MediaBrowser* browseMedia() {
        if (browser_ == nullptr) {
            browser_ = adopting(new app::MediaBrowser(this));
            connect(browser_, &app::MediaBrowser::imported, this, [this] {
                if (Status reopened = openMedia(); !reopened) {
                    app::warn(this, "Import", QString::fromStdString(reopened.error().toString()));
                }
                bin_->refresh();
                updateTitle();
            });
        }
        if (browser_->folder().empty()) {
            std::string start = std::filesystem::current_path().string();
            for (const model::MediaRef& media : document_.project().media()) {
                const std::filesystem::path parent =
                    std::filesystem::path{media.path}.parent_path();
                if (!parent.empty() && std::filesystem::is_directory(parent)) {
                    start = parent.string();
                }
            }
            static_cast<void>(browser_->showFolder(start));
        }
        browser_->show();
        browser_->raise();
        return browser_;
    }

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
    app::Hotkeys* showHotkeys() {
        if (hotkeys_ == nullptr) {
            hotkeys_ = new app::Hotkeys(actions_.keymap(), this);
            hotkeys_->setOnChanged([this] {
                applyKeymap();
                saveKeymap();
            });
        }
        hotkeys_->refresh();
        hotkeys_->show();
        hotkeys_->raise();
        return hotkeys_;
    }

    /// Gather the project's media into one folder.
    Result<io::ConsolidateReport> consolidateMedia(const std::string& destination) {
        auto report = commands::consolidateMedia(editContext(), destination);
        if (report) {
            afterMediaChange();
        }
        return report;
    }

    /// Show the transcript, and edit by it.
    app::Transcript* showTranscript() {
        if (transcript_ == nullptr) {
            transcript_ = adopting(new app::Transcript(this));
            connect(transcript_, &app::Transcript::edited, this, [this] {
                renderCache_.clear();
                timeline_->update();
                monitor_->update();
                updateCacheBar();
                updateTitle();
                refresh();
            });
            connect(transcript_, &app::Transcript::scrubbed, this,
                    [this](time::RationalTime at) { setPosition(at); });
        }
        transcript_->show();
        transcript_->raise();
        return transcript_;
    }

    /// Fit the selected music clip to a length.
    Result<render::RemixPlan> remixSelectedTo(double targetSeconds) {
        auto plan = commands::remixSelectedTo(editContext(), targetSeconds);
        if (plan) {
            afterEdit();
        }
        return plan;
    }

    /// Reframe the selected clip for the sequence's shape.
    Result<render::ReframeResult> reframeClip() {
        auto framed = commands::reframeClip(editContext());
        if (framed) {
            afterEdit();
        }
        return framed;
    }

    /// Pin the selected clip to one on a lower track, or to nothing.
    Result<model::ClipId> pinTo(model::ClipId host) {
        auto pinned = commands::pinTo(editContext(), host);
        if (pinned) {
            afterEdit();
        }
        return pinned;
    }

    /// Pin the selected clip to whatever is under it at the playhead.
    Result<model::ClipId> pinToClipBelow() {
        auto pinned = commands::pinToClipBelow(editContext());
        if (pinned) {
            afterEdit();
        }
        return pinned;
    }

    /// Save the selected title as a template.
    Status saveGraphicTemplate(const std::string& path) {
        return commands::saveGraphicTemplate(editContext(), path);
    }

    /// Drop a saved template in at the playhead.
    Result<model::ClipId> placeGraphicTemplate(const std::string& path,
                                               const time::RationalTime& at) {
        auto placed = commands::placeGraphicTemplate(editContext(), path, at);
        if (placed) {
            afterEdit();
        }
        return placed;
    }

    /// Hold a frame and show the current one against it.
    void setComparing(bool on, const time::RationalTime& reference) {
        comparing_ = on;
        referenceAt_ = reference;
        monitor_->setComparison(on, referenceAt_, compareMode_, compareSplit_);
        monitor_->update();
    }
    [[nodiscard]] bool comparing() const noexcept { return comparing_; }

    void setCompareMode(render::CompareMode mode) {
        compareMode_ = mode;
        monitor_->setComparison(comparing_, referenceAt_, compareMode_, compareSplit_);
        monitor_->update();
    }
    void setCompareSplit(double split) {
        compareSplit_ = std::clamp(split, 0.0, 1.0);
        monitor_->setComparison(comparing_, referenceAt_, compareMode_, compareSplit_);
        monitor_->update();
    }

    /// What this sequence is delivered as, and how its highlights get there.
    ///
    /// A sequence property rather than an export option, because the curve
    /// editor and the scopes are drawn against it: choosing it at export time
    /// would mean grading against one curve and delivering through another.
    void deliveryMenu() {
        const model::Sequence* sequence = liveSequence();
        if (sequence == nullptr) {
            return;
        }
        const model::Sequence::Output current = sequence->output();
        const chrome::DeliveryChoice chosen =
            chrome::deliveryMenu(current.transfer, current.highlightKnee);
        model::Sequence::Output wanted = current;
        if (chosen.transfer.has_value()) {
            wanted.transfer = *chosen.transfer;
        } else if (chosen.highlightKnee.has_value()) {
            wanted.highlightKnee = *chosen.highlightKnee;
        } else {
            return;
        }
        setDelivery(wanted);
    }

    /// Set the curve the sequence goes out through.
    bool setDelivery(const model::Sequence::Output& output) {
        if (Status set = commands::setDelivery(editContext(), output); !set) {
            app::warn(this, "Delivery", QString::fromStdString(set.error().toString()));
            return false;
        }
        // Curves, secondaries and LUTs are all baked against the delivery
        // curve, so every cached frame was made for the old one.
        afterEdit();
        return true;
    }

    /// Cut the selected clip where the picture changes.
    std::int32_t detectScenes() {
        // The dialog is the window's, and the operation's only view of it is a
        // question it asks once a frame: keep going?
        QProgressDialog progress("Looking for scene changes\u2026", "Cancel", 0, 1, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(400);
        const std::int32_t cuts =
            commands::detectScenes(editContext(), [&](std::int64_t done, std::int64_t total) {
                progress.setMaximum(static_cast<int>(total));
                progress.setValue(static_cast<int>(done));
                QCoreApplication::processEvents();
                return !progress.wasCanceled();
            });
        progress.reset();
        if (cuts > 0) {
            afterEdit();
        }
        return cuts;
    }

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
    [[nodiscard]] Status openProject(const std::string& path,
                                     Sharing sharing = Sharing::Exclusive) {
        auto opened = document_.read(path, sharing);
        if (!opened) {
            return opened.error();
        }
        document_.releaseLock();
        const bool readOnly = opened->readOnly;
        model::Project project = opened->loaded.project;
        adopt(std::move(project), std::move(opened->loaded), path, readOnly);
        document_.takeLock();
        updateTitle();
        return {};
    }

    /// Whether this window may write over the project it has open.
    [[nodiscard]] bool isReadOnly() const noexcept { return document_.isReadOnly(); }

    /// Who else has this project, if anybody. Empty when it is ours or free.
    /// Who else has this project, if anybody.
    [[nodiscard]] std::string heldBy() const { return document_.heldBy(); }

    void releaseLock() { document_.releaseLock(); }

    /// Start again, with somewhere to put something.
    void newProject() {
        model::Project fresh = model::newProject();
        adopt(std::move(fresh), io::LoadedProject{}, {});
    }

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
               bool readOnly = false) {
        document_.autosave();
        stop();

        document_.adopt(std::move(project), std::move(loaded), std::move(path));
        document_.setReadOnly(readOnly);
        sequenceId_ = document_.project().activeSequence();
        position_ =
            time::RationalTime{0, document_.project().findSequence(sequenceId_)->frameRate()};
        selectedTrack_ = model::TrackId{};
        selectedClip_ = model::ClipId{};

        // A frame cached from the project that has gone would be served for the
        // new one: the recipe covers what is in a sequence, not which project
        // it came from.
        renderCache_.clear();

        // Bound before the media is opened as well as after, so that a project
        // whose files cannot be read still leaves every panel pointed at it.
        // Otherwise the failure below shows a window full of panels describing
        // the project that has gone.
        rebindSequence();
        if (Status opened = openMedia(); !opened) {
            // The project is loaded and the window is bound to it; what failed
            // is reading its files. Said plainly rather than swallowed: a
            // timeline of clips that draw nothing is a puzzle.
            app::warn(this, "Open", QString::fromStdString(opened.error().toString()));
        }
        effects_->setSelection(model::TrackId{}, model::ClipId{});
        timeline_->setCachedSpans({});
        updateTitle();
        refresh();
        monitor_->update();
    }

    /// Write the project back where it came from.
    ///
    /// Returns false when it could not be written, so a caller that was about
    /// to do something irreversible knows not to.
    /// Write the project back where it came from.
    ///
    /// Returns false when it could not be written, so a caller that was about
    /// to do something irreversible knows not to.
    bool save() {
        if (document_.path().empty()) {
            return saveAs();
        }
        if (Status written = document_.save(); !written) {
            if (document_.isReadOnly()) {
                // Said on stderr and in the title bar, never in a dialog.
                //
                // This one was the worst offender: a project with a stale lock
                // beside it -- left by a killed process, or by a test -- turned
                // every Ctrl+S into a box somebody had to dismiss before they
                // could carry on. The window title already carries
                // "[read only]", which is where a state belongs; a refusal does
                // not also need to interrupt.
                std::fprintf(stderr, "zaro: %s\n", written.error().message().c_str());
                return false;
            }
            app::warn(this, "Save", QString::fromStdString(written.error().toString()));
            return false;
        }
        updateTitle();
        return true;
    }

    /// Save as the next version beside this one, and carry on in it.
    ///
    /// Carrying on in the new file rather than staying in the old one is the
    /// point: a version is a line somebody draws under what they had, and the
    /// next hour's work belongs after the line. The previous file is left
    /// exactly as it was, which is the other half of the point.
    /// Save as the next version beside this one, and carry on in it.
    Result<std::string> saveNewVersion() {
        auto next = document_.saveNewVersion();
        if (next) {
            updateTitle();
        }
        return next;
    }

    /// The versions beside this project, to jump between.
    void openVersionMenu() {
        if (document_.path().empty()) {
            app::say(this, "Version", "This project has not been saved yet.");
            return;
        }
        QMenu menu;
        std::map<QAction*, std::string> paths;
        for (const std::string& version : io::versionsOf(document_.path())) {
            const bool current = version == document_.path();
            QAction* action = menu.addAction(
                QString::fromStdString(std::filesystem::path{version}.filename().string()));
            action->setCheckable(true);
            action->setChecked(current);
            action->setEnabled(!current);
            paths.emplace(action, version);
        }
        if (paths.empty()) {
            menu.addAction("No other versions")->setEnabled(false);
        }
        QAction* chosen = menu.exec(QCursor::pos());
        const auto found = paths.find(chosen);
        if (found == paths.end()) {
            return;
        }
        if (Status opened = openProject(found->second); !opened) {
            app::warn(this, "Open", QString::fromStdString(opened.error().toString()));
        }
    }

    void openDialog() {
        const QString chosen =
            QFileDialog::getOpenFileName(this, "Open project", {}, "Zaro projects (*.zaro)");
        if (chosen.isEmpty()) {
            return;
        }
        const std::string path = chosen.toStdString();

        // Somebody else's lock is a question, not a refusal: often enough the
        // answer is "let me look at it anyway", and often enough the other
        // machine went home hours ago.
        if (auto held = io::readLock(path); held && !io::isOurs(*held) && !io::isStale(*held)) {
            QMessageBox ask{this};
            ask.setWindowTitle("Open project");
            ask.setText(
                QString("%1 has this project open on %2.")
                    .arg(QString::fromStdString(held->user), QString::fromStdString(held->host)));
            ask.setInformativeText(
                "Opening it read-only lets you look without saving over "
                "their work.");
            QPushButton* readOnly = ask.addButton("Open Read-Only", QMessageBox::AcceptRole);
            QPushButton* takeOver = ask.addButton("Take Over", QMessageBox::DestructiveRole);
            ask.addButton(QMessageBox::Cancel);
            ask.exec();

            Sharing sharing = Sharing::Exclusive;
            if (ask.clickedButton() == readOnly) {
                sharing = Sharing::ReadOnly;
            } else if (ask.clickedButton() == takeOver) {
                sharing = Sharing::TakeOver;
            } else {
                return;
            }
            if (Status opened = openProject(path, sharing); !opened) {
                app::warn(this, "Open", QString::fromStdString(opened.error().toString()));
            }
            return;
        }

        if (Status opened = openProject(path); !opened) {
            app::warn(this, "Open", QString::fromStdString(opened.error().toString()));
        }
    }

    bool saveAs() {
        const QString chosen = QFileDialog::getSaveFileName(
            this, "Save project",
            QString::fromStdString(document_.path().empty() ? "project.zaro" : document_.path()),
            "Zaro projects (*.zaro)");
        if (chosen.isEmpty()) {
            return false;
        }
        if (Status written = document_.saveAs(chosen.toStdString()); !written) {
            app::warn(this, "Save", QString::fromStdString(written.error().toString()));
            return false;
        }
        updateTitle();
        return true;
    }

    /// Write the recovery file, if there is anything to recover.
    void autosave() { document_.autosave(); }

    [[nodiscard]] const std::string& projectPath() const noexcept { return document_.path(); }

    /// Point Save at a different file. What Save As does once somebody has
    /// chosen one.
    /// Point Save at a different file.
    void setProjectPath(std::string path) {
        document_.setPath(std::move(path));
        updateTitle();
    }

    /// The file name, and whether it differs from what is on disk.
    void updateTitle() {
        const QString name = document_.path().empty()
                                 ? QString{"Untitled"}
                                 : QFileInfo(QString::fromStdString(document_.path())).fileName();
        // Said in the title, because read-only is a fact about the whole
        // window and finding out at the moment of saving is finding out too
        // late.
        setWindowTitle(QString("%1%2%3 — Zaro")
                           .arg(name, document_.commands().isModified() ? "*" : "",
                                document_.isReadOnly() ? " [read only]" : ""));
        if (bars_.statusLeft != nullptr) {
            updateChrome();
        }
    }

    /// Show the source frame the picture is currently made from.
    void matchFrame() {
        auto found = commands::frameToMatch(editContext());
        if (!found) {
            return;
        }
        const model::MediaRef* ref = document_.project().findMedia(found->media);
        if (ref == nullptr) {
            return;
        }
        source_->showFrame(*ref, found->at);
        setSourceShown(true);
    }

    /// Make a subclip of what is marked in the source monitor.
    void makeSubclip() {
        const auto range = source_->markedRange();
        if (!range || !source_->media().isValid()) {
            return;
        }
        if (commands::makeSubclip(editContext(), source_->media(), *range)) {
            bin_->refresh();
        }
    }

    /// Point the selected clip at a different file.
    void replaceSelectedSource(model::MediaRefId media) {
        if (Status replaced = commands::replaceSelectedSource(editContext(), media); !replaced) {
            app::warn(this, "Replace footage", QString::fromStdString(replaced.error().toString()));
            return;
        }
        afterEdit();
    }

    /// Line the multicam angles up, and say which ones would not.
    void syncAngles(bool byEar) {
        auto report = commands::syncAngles(editContext(), byEar);
        if (!report) {
            app::warn(this, "Multicam", QString::fromStdString(report.error().toString()));
            return;
        }
        if (report->synced > 0) {
            afterEdit();
        }
        lastSyncCount_ = report->synced;
        lastSyncSkipped_ = static_cast<std::int32_t>(report->skipped.size());
        if (report->skipped.empty()) {
            return;
        }
        QStringList lines;
        for (const std::string& line : report->skipped) {
            lines.append(QString::fromStdString(line));
        }
        app::say(this, "Multicam",
                 QString("Synced %1 of %2 angles.\n\nNot synced:\n%3")
                     .arg(report->synced)
                     .arg(report->total)
                     .arg(lines.join("\n")));
    }

    [[nodiscard]] std::int32_t lastSyncCount() const noexcept { return lastSyncCount_; }
    [[nodiscard]] std::int32_t lastSyncSkipped() const noexcept { return lastSyncSkipped_; }

    /// Recompute what the cache bar shows.
    ///
    /// Sampled at one point per pixel of the timeline's width: the bar cannot
    /// show more than that, and checking every frame of a long sequence would
    /// hash the whole timeline on every repaint.
    void updateCacheBar() {
        const model::Sequence* sequence = liveSequence();
        if (sequence == nullptr) {
            return;
        }
        const time::TimeRange visible = timeline_->layout().visibleRange(sequence->frameRate());
        timeline_->setCachedSpans(render::cachedSpans(renderCache_, &document_.project(), *sequence,
                                                      visible, timeline_->width()));
    }

    /// The work behind the menu entry, separated from the menu so that it can
    /// be driven without one.
    void renderVisibleRange() {
        const model::Sequence* sequence = liveSequence();
        if (sequence == nullptr || media_ == nullptr) {
            return;
        }
        const time::TimeRange visible = timeline_->layout().visibleRange(sequence->frameRate());
        if (visible.isEmpty()) {
            return;
        }

        QProgressDialog progress("Rendering…", "Cancel", 0,
                                 static_cast<int>(visible.duration().frames()), this);
        progress.setWindowModality(Qt::WindowModal);
        // A pre-render of a few frames is not worth a dialog appearing and
        // vanishing; one of a few hundred is.
        progress.setMinimumDuration(400);

        render::RenderGraph graph{*media_};
        graph.setProject(&document_.project());
        graph.setTextRasterizer(&text_);
        graph.setRenderCache(&renderCache_);
        auto stats = render::prerender(graph, renderCache_, &document_.project(), *sequence,
                                       visible, [&progress](std::int32_t done, std::int32_t total) {
                                           progress.setMaximum(total);
                                           progress.setValue(done);
                                           QCoreApplication::processEvents();
                                           return !progress.wasCanceled();
                                       });
        progress.reset();
        if (!stats) {
            app::warn(this, "Render", QString::fromStdString(stats.error().toString()));
            return;
        }
        updateCacheBar();
    }

    Status openMedia() {
        auto opened = platform::ffmpeg::ProjectMediaSource::open(document_.project());
        if (!opened) {
            return opened.error();
        }
        media_ = std::move(*opened);
        // The project this opened media for may be a different one -- adopt
        // calls through here -- so the panels are pointed at it again, along
        // with the monitor.
        rebindSequence();
        effects_->setAudioSource(media_.get());
        source_->setProvider(media_.get());
        startWaveforms();
        bars_.scrubber->setRange(0, static_cast<int>(liveSequence()->duration().frames()));
        refresh();
        return {};
    }

    void setPosition(const time::RationalTime& position) {
        const std::int64_t last =
            std::max<std::int64_t>(0, liveSequence()->duration().frames() - 1);
        const std::int64_t clamped = std::clamp<std::int64_t>(position.frames(), 0, last);
        position_ = time::RationalTime{clamped, liveSequence()->frameRate()};
        monitor_->setPosition(position_);
        timeline_->setPlayhead(position_);
        // The panel shows values at the playhead and writes keyframes there, so
        // it has to know where the playhead is.
        effects_->setPosition(position_);
        refreshInstruments();
        refresh();
    }

    void step(std::int64_t frames) {
        stop();
        setPosition(position_ + time::RationalTime{frames, liveSequence()->frameRate()});
    }

protected:
    /// Turn a key press into the same text a keymap holds.
    ///
    /// Qt's own portable spelling, run through the keymap's normaliser, so
    /// there is one form and one comparison rather than a second opinion about
    /// what "Shift+Left" is called.
    [[nodiscard]] static std::string keystrokeOf(const QKeyEvent* event) {
        switch (event->key()) {
            case Qt::Key_Control:
            case Qt::Key_Shift:
            case Qt::Key_Alt:
            case Qt::Key_Meta:
                return {};  // a modifier on its own is not a keystroke
            default:
                break;
        }
        const QKeySequence sequence{event->keyCombination()};
        auto normalised =
            ui::normaliseShortcut(sequence.toString(QKeySequence::PortableText).toStdString());
        return normalised ? *normalised : std::string{};
    }

    void keyPressEvent(QKeyEvent* event) override {
        // Whatever the keymap says this keystroke is, whether or not it has a
        // menu item behind it. There used to be a table here of "the bindings
        // that are not menu items" and a switch for playback underneath it,
        // which meant three places knew what a key did and only one of them
        // could be changed. Now there is one: the keymap.
        const std::string keystroke = keystrokeOf(event);
        const std::string wanted =
            keystroke.empty() ? std::string{} : actions_.keymap().actionFor(keystroke);
        if (!wanted.empty() && trigger(wanted)) {
            return;
        }
        QWidget::keyPressEvent(event);
    }

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
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == monitor_ && event->type() == QEvent::Resize) {
            maskOverlay_->setGeometry(monitor_->rect());
            viewerOverlay_->setGeometry(monitor_->rect());
        }
        return QWidget::eventFilter(watched, event);
    }

    void closeEvent(QCloseEvent* event) override {
        // Handed back on the way out, so the next person is not told a machine
        // that has gone home still has it.
        releaseLock();
        // Never a dialog on the way out.
        //
        // "You have unsaved changes -- save?" is a question whose answer is
        // almost always yes, asked at the moment somebody has already decided
        // to leave. Writing the recovery file instead means quitting is always
        // instant and never loses anything: the next open finds the autosave
        // and offers it back. It also keeps the promise that the file somebody
        // last chose to save stays as they left it.
        autosave();
        saveWorkspace();
        shutDown();
        QWidget::closeEvent(event);
    }

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
    QAction* menuItem(QMenu* menu, const char* actionId) {
        QAction* action = actions_.action(actionId);
        menu->addAction(action);
        return action;
    }

    /// The commands that arrive by key rather than through a menu.
    void bindPlaybackActions() {
        bindAction("play-pause", [this] { togglePlay(); });
        bindAction("shuttle-back", [this] {
            playback_.transport().pressJ();
            startIfPlaying();
        });
        bindAction("shuttle-stop", [this] {
            playback_.transport().pressK();
            stop();
        });
        bindAction("shuttle-forward", [this] {
            playback_.transport().pressL();
            startIfPlaying();
        });
        bindAction("step-back", [this] { step(-1); });
        bindAction("step-forward", [this] { step(1); });
        bindAction("go-to-start", [this] {
            stop();
            setPosition(time::RationalTime{0, liveSequence()->frameRate()});
        });
        bindAction("go-to-end", [this] {
            stop();
            setPosition(liveSequence()->duration());
        });
        bindAction("mark-in", [this] { doMarkIn(); });
        bindAction("mark-out", [this] { doMarkOut(); });
        bindAction("insert-from-source", [this] { doInsert(); });
        bindAction("overwrite-from-source", [this] { doOverwrite(); });
        bindAction("source-back", [this] { doSourceBack(); });
        bindAction("source-forward", [this] { doSourceForward(); });
    }

    /// Say what every command does, once.
    ///
    /// The menus and the bars name ids and nothing else; this is the only place
    /// that knows what an id means. It used to be spread through buildMenus as a
    /// lambda per menu item, which is why building a menu needed the window:
    /// naming File > Save meant writing out what saving is.
    void bindCommands() {
        actions_.bind("new-project", [this] { newProject(); });
        actions_.bind("open-project", [this] { openDialog(); });
        actions_.bind("save-project", [this] { static_cast<void>(save()); });
        actions_.bind("save-project-as", [this] { static_cast<void>(saveAs()); });
        actions_.bind("save-version", [this] {
            auto saved = saveNewVersion();
            if (!saved) {
                app::say(this, "Version", QString::fromStdString(saved.error().message()));
                return;
            }
            // Said out loud: the window title changes too, but a version
            // that appeared to do nothing is one people press twice.
            app::say(this, "Version",
                     QString("Now working in %1")
                         .arg(QString::fromStdString(
                             std::filesystem::path{*saved}.filename().string())));
        });
        actions_.bind("open-version", [this] { openVersionMenu(); });
        actions_.bind("import-media", [this] { bin_->importFiles(); });
        actions_.bind("browse-media", [this] { browseMedia(); });
        actions_.bind("relink-media", [this] { relinkDialog(); });
        actions_.bind("consolidate-media", [this] { consolidateDialog(); });
        actions_.bind("export-sequence", [this] { exportDialog(); });
        actions_.bind("export-otio", [this] { exportOtio(); });
        actions_.bind("save-template", [this] { saveTemplateDialog(); });
        actions_.bind("place-template", [this] { placeTemplateDialog(); });
        actions_.bind("close-window", [this] { close(); });
        actions_.bind("undo", [this] { timeline_->undo(); });
        actions_.bind("redo", [this] { timeline_->redo(); });
        actions_.bind("select-all", [this] { timeline_->selectAll(); });
        actions_.bind("detect-scenes", [this] { static_cast<void>(detectScenes()); });
        actions_.bind("match-frame", [this] { matchFrame(); });
        actions_.bind("make-subclip", [this] { makeSubclip(); });
        actions_.bind("proxies", [this] { proxyMenu(); });
        actions_.bind("multicam", [this] { multicamMenu(); });
        actions_.bind("captions", [this] { captionsMenu(); });
        actions_.bind("razor", [this] { timeline_->razorAtPlayhead(); });
        actions_.bind("add-dissolve", [this] { timeline_->addDissolveAtPlayhead(); });
        actions_.bind("render-range", [this] { renderMenu(); });
        actions_.bind("delivery", [this] { deliveryMenu(); });
        actions_.bind("loudness", [this] { loudnessMenu(); });
        actions_.bind("show-transcript", [this] { showTranscript(); });
        actions_.bind("fit-music", [this] {
            const model::Sequence* sequence = liveSequence();
            if (sequence == nullptr) {
                return;
            }
            // The picture's length, less wherever the music starts: what
            // has to be filled is what is left after it comes in.
            const model::Track* track = sequence->findTrack(selectedTrack_);
            const model::Clip* clip = track != nullptr ? track->find(selectedClip_) : nullptr;
            const double from = clip != nullptr ? clip->start().toSecondsDouble() : 0.0;
            const double wanted = sequence->duration().toSecondsDouble() - from;
            auto plan = remixSelectedTo(wanted);
            if (!plan) {
                app::say(this, "Fit music", QString::fromStdString(plan.error().message()));
                return;
            }
            app::say(this, "Fit music",
                     QString("Cut %1 beats out at %2s, now %3s long.")
                         .arg(plan->beatsRemoved)
                         .arg(plan->cutAt, 0, 'f', 2)
                         .arg(plan->seconds, 0, 'f', 2));
        });
        actions_.bind("add-marker", [this] { timeline_->addMarkerAtPlayhead(); });
        actions_.bind("next-marker", [this] { doNextMarker(); });
        actions_.bind("previous-marker", [this] { doPreviousMarker(); });
        actions_.bind("resolve-comment", [this] { static_cast<void>(toggleCommentHere()); });
        actions_.bind("export-review", [this] { exportReviewNotes(); });
        actions_.bindToggle("compare", [this](bool on) {
            // Turning it on takes the frame showing now as the reference. That
            // is the gesture: somebody looks at a shot they like and says
            // "against this" -- asking them to nominate one first would be a
            // step between the thought and the thing.
            setComparing(on, on ? position_ : referenceAt_);
        });
        actions_.bind("match-shot", [this] { matchShot(); });
        actions_.bind("zoom-in", [this] { timeline_->zoomBy(1.4); });
        actions_.bind("zoom-out", [this] { timeline_->zoomBy(1.0 / 1.4); });
        actions_.bind("zoom-fit", [this] { timeline_->zoomToFit(); });
        actions_.bindToggle("safe-guides", [this](bool on) {
            viewerOverlay_->setGuides(on);
            if (bars_.guidesButton != nullptr) {
                bars_.guidesButton->setChecked(on);
            }
        });
        actions_.bind("reset-panels", [this] { setWorkspace(workspace_); });
        actions_.bind("hotkeys", [this] { showHotkeys(); });
        actions_.bind("about", [this] {
            QMessageBox::about(this, "Zaro Video",
                               "Zaro Video — a non-linear editor.\n\n"
                               "C++20, Qt 6, FFmpeg, GPU compositing on Qt RHI.");
        });
    }

    void buildMenus() {
        bars_.menuBar = chrome::buildMenuBar(
            this, actions_, kWorkspaces, bars_.workspaceActions,
            [this](const QString& name) { setWorkspace(name); },
            [this](QMenuBar* bar) {
                QMenu* window = bar->addMenu("Window");
                panelAction(window, "Project Bin", [this] { return bin_; });
                panelAction(window, "Effect Controls",
                            [this] { return static_cast<QWidget*>(effects_); });
                panelAction(window, "Scopes", [this] { return static_cast<QWidget*>(scopes_); });
                panelAction(window, "Audio Mixer",
                            [this] { return static_cast<QWidget*>(mixer_); });
                window->addSeparator();
                menuItem(window, "reset-panels");

                QMenu* help = bar->addMenu("Help");
                menuItem(help, "hotkeys");
                menuItem(help, "about");
            });
    }

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
    [[nodiscard]] chrome::Hooks chromeHooks() {
        chrome::Hooks hooks;
        hooks.chooseWorkspace = [this](const QString& name) { setWorkspace(name); };
        hooks.chooseTool = [this](app::TimelineWidget::Tool tool) { timeline_->setTool(tool); };
        hooks.showSource = [this](bool on) { setSourceShown(on); };
        hooks.showProgram = [this](bool on) { setProgramShown(on); };
        hooks.setGuides = [this](bool on) { viewerOverlay_->setGuides(on); };
        hooks.setSnapEnabled = [this](bool on) { timeline_->setSnapEnabled(on); };
        hooks.setZoomFraction = [this](double fraction) { timeline_->setZoomFraction(fraction); };
        hooks.queueRender = [this] { deliver_->queueCurrent(); };
        hooks.toggleRendering = [this] {
            deliver_->toggleRendering();
            updateChrome();
        };
        return hooks;
    }

    QWidget* buildToolBar() {
        return chrome::buildToolBar(this, bars_, actions_, chromeHooks(), kWorkspaces, kSupportUrl);
    }

    QWidget* buildViewerBar() {
        QWidget* bar = chrome::buildViewerBar(this, bars_, actions_, chromeHooks());
        syncViewers();
        return bar;
    }

    QWidget* buildTransportBar() { return chrome::buildTransportBar(this, bars_, actions_); }

    QWidget* buildTimelinePane() {
        return chrome::buildTimelinePane(this, bars_, actions_, chromeHooks(), timeline_);
    }

    QWidget* buildStatusBar() { return chrome::buildStatusBar(this, bars_); }

    /// Show what is turned on, and say so on the toggles and in the well.
    ///
    /// One place, so the toggles and the monitors cannot disagree: every way in
    /// -- a click on a chip, a clip opened from the bin, a match frame -- ends
    /// up here rather than setting visibility itself.
    void syncViewers() {
        source_->setVisible(sourceShown_);
        monitor_->setVisible(programShown_);
        bars_.noMonitorLabel->setVisible(!sourceShown_ && !programShown_);
        // These do not re-enter: setChecked only emits when the value moves,
        // and by here it already is what it is being set to.
        bars_.sourceTab->setChecked(sourceShown_);
        bars_.programTab->setChecked(programShown_);
        bars_.sourceTab->setIcon(app::icons::toolIcon(
            sourceShown_ ? app::icons::Glyph::CheckCircle : app::icons::Glyph::Circle, 13));
        bars_.programTab->setIcon(app::icons::toolIcon(
            programShown_ ? app::icons::Glyph::CheckCircle : app::icons::Glyph::Circle, 13));
    }

public:
    /// Turn the source monitor on. Opening a clip from the bin and match frame
    /// both want it up; neither wants the program taken down to get it, which
    /// is what a stack used to make them do.
    void setSourceShown(bool on) {
        sourceShown_ = on;
        syncViewers();
    }
    void setProgramShown(bool on) {
        programShown_ = on;
        syncViewers();
    }

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
    static QString layoutKey(const QString& workspace, const char* which) {
        return QString("workspace/%1/%2-v3").arg(workspace, QString::fromUtf8(which));
    }

    /// A workspace is which panels are up. Four arrangements, because there are
    /// four things people do with an editor, and each of them wants a different
    /// half of the window: the panels a colourist needs are dead weight while
    /// somebody is assembling, and the reverse.
    void setWorkspace(const QString& name) {
        if (!kWorkspaces.contains(name)) {
            return;
        }
        // The arrangement of the workspace being left is remembered, so coming
        // back to it finds the splitters where they were.
        if (topSplitter_ != nullptr && !workspace_.isEmpty()) {
            QSettings settings("Zaro", "Zaro Video");
            settings.setValue(layoutKey(workspace_, "top"), topSplitter_->saveState());
            settings.setValue(layoutKey(workspace_, "main"), mainSplitter_->saveState());
        }
        workspace_ = name;

        const bool colour = name == "Color";
        const bool audio = name == "Audio";
        const bool deliver = name == "Deliver";
        if (bars_.workspaceStack != nullptr) {
            bars_.workspaceStack->setCurrentIndex(deliver ? 1 : 0);
            bars_.actionStack->setCurrentIndex(deliver ? 1 : 0);
            if (deliver) {
                deliver_->setPlayhead(position_);
            }
        }
        // The bin and the parameter panel are both about picture; Audio has a
        // console in the middle and a channel's chain on the right, and neither
        // of those wants a clip's motion controls beside it.
        bin_->setVisible(!colour && !deliver && !audio);
        effects_->setVisible(!deliver && !audio);
        scopes_->setVisible(colour);
        mixer_->setVisible(audio);
        // Color is a different room: the gallery and the shot strip replace the
        // bin and the timeline, the grade chain sits over the parameters, and
        // the wheels take the bottom of the window.
        gallery_->setVisible(colour);
        clipStrip_->setVisible(colour);
        bars_.nodesBox->setVisible(colour);
        palette_->setVisible(colour);
        bars_.timelinePane->setVisible(!colour && !deliver);
        // Audio is a console: the mixer takes the centre, the loudness meter
        // and the channel's chain take the sides, and the picture stands down.
        bars_.audioSide->setVisible(audio);
        channel_->setVisible(audio);
        bars_.viewerWell->setVisible(!audio && !deliver);
        bars_.viewerBar->setVisible(!audio && !deliver);
        if (audio) {
            channel_->setTrack(mixer_->picked());
            refreshInstruments();
        }
        if (colour) {
            palette_->setSelection(selectedTrack_, selectedClip_);
            clipStrip_->setSelection(selectedTrack_, selectedClip_);
            refreshGradeChain();
        }

        for (auto entry = bars_.workspaceTabs.constBegin(); entry != bars_.workspaceTabs.constEnd();
             ++entry) {
            entry.value()->setChecked(entry.key() == name);
        }
        for (auto entry = bars_.workspaceActions.constBegin();
             entry != bars_.workspaceActions.constEnd(); ++entry) {
            entry.value()->setChecked(entry.key() == name);
        }

        QSettings settings("Zaro", "Zaro Video");
        if (const auto state = settings.value(layoutKey(name, "top")).toByteArray();
            !state.isEmpty()) {
            topSplitter_->restoreState(state);
        }
        if (const auto state = settings.value(layoutKey(name, "main")).toByteArray();
            !state.isEmpty()) {
            mainSplitter_->restoreState(state);
        }
        updateChrome();
    }

private:
    /// Everything in the chrome that describes state rather than causing it.
    /// Gather what the bars say, and hand it to the code that says it.
    ///
    /// The gathering is the window's -- these facts live in eight different
    /// places and only the window knows all of them. The saying is not, and
    /// used to be: fourteen setText calls formatting strings out of a project,
    /// a timeline widget, a bin and a render queue. See chrome::refresh.
    void updateChrome() {
        const model::Sequence* sequence = liveSequence();
        static const QString kToolNames[] = {"Select", "Blade", "Trim", "Slip", "Hand", "Zoom"};

        chrome::Status status;
        status.projectName =
            document_.path().empty()
                ? QString{"Untitled"}
                : QFileInfo(QString::fromStdString(document_.path())).completeBaseName();
        status.haveSequence = sequence != nullptr;
        status.modified = document_.commands().isModified();
        if (sequence != nullptr) {
            status.sequenceName = QString::fromStdString(sequence->name());
            status.width = sequence->width();
            status.height = sequence->height();
            status.frameRate = sequence->frameRate().toDouble();
            const bool dropFrame = time::supportsDropFrame(sequence->frameRate());
            status.durationTimecode =
                QString::fromStdString(time::timecodeFromFrames(sequence->duration().frames(),
                                                                sequence->frameRate(), dropFrame)
                                           .toString());
        }
        status.comparing = monitor_->comparing();
        status.toolIndex = static_cast<std::size_t>(timeline_->tool());
        status.toolName = kToolNames[status.toolIndex];
        status.workspace = workspace_;
        status.binItems = bin_->count();
        status.snapEnabled = timeline_->snapEnabled();
        status.zoomFraction = timeline_->zoomFraction();
        status.inDeliver = workspace_ == "Deliver" && deliver_ != nullptr;
        if (status.inDeliver) {
            status.deliverStatus = deliver_->statusSummary();
            status.deliverRange = deliver_->rangeSummary();
            status.rendering = deliver_->rendering();
        }
        status.platformLabel = kPlatformLabel;

        chrome::refresh(bars_, status);
    }

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

    void goToStart() {
        stop();
        setPosition(time::RationalTime{0, liveSequence()->frameRate()});
    }
    void goToEnd() {
        stop();
        setPosition(liveSequence()->duration());
    }
    void stepBack() { step(-1); }
    void stepForward() { step(1); }

    /// Panel sizes and window geometry, remembered between sessions.
    ///
    /// Saved on close rather than continuously: writing settings on every drag
    /// of a splitter is a lot of disk traffic for something only read once.
    void saveWorkspace() {
        QSettings settings("Zaro", "Zaro Video");
        settings.setValue("window/geometry", saveGeometry());
        // Per workspace, because the panels differ between them: one saved
        // arrangement restored into a different set of visible panels is a
        // collapsed bin and a mixer four pixels tall.
        settings.setValue("workspace/current", workspace_);
        settings.setValue(layoutKey(workspace_, "top"), topSplitter_->saveState());
        settings.setValue(layoutKey(workspace_, "main"), mainSplitter_->saveState());
    }

    void restoreWorkspace() {
        QSettings settings("Zaro", "Zaro Video");
        // Each restored only if it was stored, so a first run gets the
        // stretch factors set above rather than a collapsed layout.
        if (const auto geometry = settings.value("window/geometry").toByteArray();
            !geometry.isEmpty()) {
            restoreGeometry(geometry);
        }
        const QString wanted = settings.value("workspace/current", "Edit").toString();
        // Always through setWorkspace, so the panels, the tabs and the splitter
        // states are one decision rather than three that can disagree.
        workspace_.clear();
        setWorkspace(kWorkspaces.contains(wanted) ? wanted : QString{"Edit"});
    }

    /// Stop everything and join. Called from both the close event and the
    /// destructor, because they are not the same path: quitting with Cmd+Q
    /// destroys the window without ever delivering a close event, and a
    /// std::thread destroyed while still joinable calls std::terminate. That
    /// was a real crash -- the application aborted on quit whenever a waveform
    /// scan was still running, which on a freshly opened project is always.
    void shutDown() {
        shuttingDown_.store(true, std::memory_order_relaxed);
        stop();
        if (waveformThread_.joinable()) {
            waveformThread_.join();
        }
    }

    void doNextMarker() {
        if (const model::Marker* marker = liveSequence()->markerAfter(position_)) {
            stop();
            setPosition(marker->range.start());
        }
    }
    void doPreviousMarker() {
        if (const model::Marker* marker = liveSequence()->markerBefore(position_)) {
            stop();
            setPosition(marker->range.start());
        }
    }

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
    void placeFromSource(edit::PlaceMode mode) {
        const auto range = source_->markedRange();
        if (!range || !source_->media().isValid()) {
            return;
        }
        const auto& videoTracks = liveSequence()->videoTracks();
        if (videoTracks.empty()) {
            return;
        }
        auto built = edit::makePlaceFromSource(document_.project(),
                                               {liveSequence()->id(), videoTracks.front().id()},
                                               source_->media(), *range, position_, mode);
        if (!built) {
            return;
        }
        document_.commands().execute(document_.project(), std::move(*built));
        document_.commands().breakMerge();
        bars_.scrubber->setRange(0, static_cast<int>(liveSequence()->duration().frames()));
        timeline_->update();
        monitor_->update();
        refresh();
    }

    void togglePlay() { playback_.togglePlay(position_); }

    void startIfPlaying() { playback_.startIfPlaying(position_); }

    void stop() { playback_.stop(); }

    /// Peaks are generated off the UI thread: decoding a long file's audio
    /// takes seconds, and a project that freezes while it opens is worse than
    /// one whose waveforms arrive a moment late.
    void startWaveforms() {
        // A scan may already be running -- reopening the media does this, which
        // is what switching to proxies does. Assigning over a joinable thread
        // is an immediate std::terminate, and that is exactly how it presented:
        // the application aborted the moment proxies were switched on.
        if (waveformThread_.joinable()) {
            waveformThread_.join();
        }

        const std::filesystem::path cacheDirectory =
            std::filesystem::temp_directory_path() / "zaro" / "waveforms";

        // Every media reference is tried, rather than only those the project
        // file says have audio. That cached info is a cache -- it can be stale,
        // absent, or written by a build that did not record it -- and deciding
        // whether to look at a file based on it means a missing field silently
        // becomes a missing waveform. Files without audio simply fail and are
        // skipped.
        std::vector<std::pair<model::MediaRefId, std::string>> wanted;
        for (const model::MediaRef& ref : document_.project().media()) {
            wanted.emplace_back(ref.id, ref.path);
        }
        if (wanted.empty()) {
            return;
        }

        waveformThread_ = std::thread{[this, wanted, cacheDirectory] {
            platform::ffmpeg::WaveformStore store{cacheDirectory.string()};
            const auto keepGoing = [this] {
                return !shuttingDown_.load(std::memory_order_relaxed);
            };
            for (const auto& [id, path] : wanted) {
                if (!keepGoing()) {
                    return;
                }
                // Cancellable mid-file, not just between files: one long clip
                // is the common case, so checking only at file boundaries would
                // still make quitting wait for the whole scan.
                auto built = store.get(path, 512, keepGoing);
                if (!built) {
                    continue;
                }
                auto shared = std::make_shared<const media::Waveform>(std::move(*built));
                // Back to the UI thread to hand it over: the widget is not
                // thread safe and neither is repainting.
                QMetaObject::invokeMethod(
                    this, [this, id, shared] { timeline_->setWaveform(id, shared); },
                    Qt::QueuedConnection);
            }
        }};
    }

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
    void refreshInstruments() {
        if (media_ == nullptr || liveSequence() == nullptr) {
            return;
        }
        const bool wantScopes = scopes_ != nullptr && scopes_->wantsMeasurement();
        const bool wantThumb = thumb_ != nullptr && thumb_->isVisible();
        if (playback_.isPlaying() || (!wantScopes && !wantThumb)) {
            return;
        }
        render::RenderGraph graph{*media_};
        graph.setTextRasterizer(&text_);
        graph.setProject(&document_.project());
        auto frame = graph.composite(*liveSequence(), position_);
        if (!frame) {
            if (wantScopes) {
                scopes_->clear();
            }
            if (wantThumb) {
                thumb_->clearFrame();
            }
            return;
        }

        if (wantScopes) {
            render::ScopeOptions options;
            options.waveformColumns = std::max(64, scopes_->width());
            // Every second row. The shape of a waveform does not change for
            // being measured at half the vertical resolution, and this is
            // running between scrubs.
            options.rowStride = 2;
            scopes_->setScopes(render::measure(*frame, options));
        }
        if (wantThumb) {
            showThumbnail(*frame);
        }
    }

    /// The composited frame, encoded for display and handed to the thumbnail.
    void showThumbnail(const render::RgbaImage& frame) {
        const int wide = frame.width();
        const int tall = frame.height();
        if (wide <= 0 || tall <= 0) {
            thumb_->clearFrame();
            return;
        }
        const int stride = wide * 3;
        std::vector<std::uint8_t> rgb(static_cast<std::size_t>(stride) *
                                      static_cast<std::size_t>(tall));
        if (!render::toDisplayRgb24(frame, rgb.data(), stride)) {
            thumb_->clearFrame();
            return;
        }
        // Copied, because the QImage above only borrows the vector, and the
        // vector is gone at the end of this function.
        thumb_->setFrame(QImage{rgb.data(), wide, tall, stride, QImage::Format_RGB888}.copy());

        const model::Sequence* sequence = liveSequence();
        const model::Track* track = sequence != nullptr && !sequence->videoTracks().empty()
                                        ? &sequence->videoTracks().front()
                                        : nullptr;
        const model::Clip* clip = track != nullptr ? track->clipAt(position_) : nullptr;
        const bool dropFrame =
            sequence != nullptr && time::supportsDropFrame(sequence->frameRate());
        thumb_->setCaption(
            clip != nullptr ? QString::fromStdString(clip->name) : QString{"—"},
            sequence != nullptr
                ? QString::fromStdString(
                      time::timecodeFromFrames(position_.frames(), sequence->frameRate(), dropFrame)
                          .toString())
                : QString{});
    }

    /// Measure the whole programme's loudness.
    ///
    /// On demand, not continuously: the reading that matters is the gated
    /// integrated figure over everything, and that means mixing the sequence
    /// from end to end. Doing it every time a fader moved would make the mixer
    /// unusable to pay for a number nobody reads until delivery.
    void measureProgramme() {
        const model::Sequence* sequence = liveSequence();
        if (sequence == nullptr) {
            return;
        }
        QApplication::setOverrideCursor(Qt::WaitCursor);
        render::AudioGraph graph{*media_};
        const time::TimeRange whole{time::RationalTime{0, sequence->frameRate()},
                                    sequence->duration()};
        auto measured = graph.measureLoudness(*sequence, whole);
        QApplication::restoreOverrideCursor();
        if (!measured) {
            app::warn(this, "Loudness", QString::fromStdString(measured.error().message()));
            return;
        }
        loudness_->setMeasurement(*measured);
    }

    /// Say which stages of the chain this shot has been through.
    ///
    /// Read from the clip rather than remembered, for the same reason the
    /// panels are: after an undo the clip is the only thing that knows.
    void refreshGradeChain() {
        const model::Sequence* sequence = liveSequence();
        const model::Track* track =
            sequence != nullptr ? sequence->findTrack(selectedTrack_) : nullptr;
        const model::Clip* clip = track != nullptr ? track->find(selectedClip_) : nullptr;
        nodes_->setEnabledChain(clip != nullptr);
        if (clip == nullptr) {
            nodes_->setOccupied({false, false, false, false});
            return;
        }
        nodes_->setOccupied({!clip->color.isIdentity() || !clip->wheels.isIdentity(),
                             !clip->curves.isIdentity(), clip->secondary.qualifier.enabled,
                             !clip->lut.path.empty()});
    }

    /// Keep the frame that is on screen, as a reference to grade against.
    ///
    /// Grabbed off the monitor rather than composited again: what somebody
    /// means by "this frame" is the one they are looking at, and rendering a
    /// second one would be a different picture the moment anything about the
    /// grade or the proxy setting differed.
    void grabStill() {
        const QImage shot = monitor_->grab().toImage();
        if (shot.isNull()) {
            return;
        }
        const model::Sequence* sequence = liveSequence();
        const bool dropFrame =
            sequence != nullptr && time::supportsDropFrame(sequence->frameRate());
        const QString name =
            sequence != nullptr
                ? QString::fromStdString(
                      time::timecodeFromFrames(position_.frames(), sequence->frameRate(), dropFrame)
                          .toString())
                : QString{"still"};
        gallery_->addStill(shot, position_, name);
    }

    /// Put a .cube on the selected shot.
    void applyLookToSelection(const QString& path) {
        if (!selectedClip_.isValid()) {
            app::say(this, "Look", "Pick a shot first — a look goes on a clip.");
            return;
        }
        model::LutRef look;
        look.path = path.toStdString();
        look.amount = 1.0;
        auto built = edit::makeSetLut(document_.project(), {sequenceId_, selectedTrack_},
                                      selectedClip_, look);
        if (!built) {
            return;
        }
        document_.commands().execute(document_.project(), std::move(*built));
        document_.commands().breakMerge();
        renderCache_.clear();
        effects_->refresh();
        clipStrip_->refresh();
        refreshGradeChain();
        monitor_->update();
        refreshInstruments();
        updateTitle();
    }

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

    void refresh() {
        if (liveSequence() == nullptr) {
            return;
        }
        const bool dropFrame = time::supportsDropFrame(liveSequence()->frameRate());
        const time::Timecode code =
            time::timecodeFromFrames(position_.frames(), liveSequence()->frameRate(), dropFrame);
        bars_.timecode->setText(QString::fromStdString(code.toString()));
        if (!bars_.scrubber->isSliderDown()) {
            bars_.scrubber->setValue(static_cast<int>(position_.frames()));
        }

        if (deliver_ != nullptr) {
            deliver_->setPlayhead(position_);
        }

        const time::Timecode left = time::timecodeFromFrames(
            std::max<std::int64_t>(0, liveSequence()->duration().frames() - position_.frames()),
            liveSequence()->frameRate(), dropFrame);
        bars_.remaining->setText("-" + QString::fromStdString(left.toString()));

        const model::Track* track = liveSequence()->findTrack(selectedTrack_);
        const model::Clip* clip = track != nullptr ? track->find(selectedClip_) : nullptr;
        viewerOverlay_->setInfo(
            clip != nullptr ? QString::fromStdString(clip->name) : QString{},
            QString::fromStdString(code.toString()),
            QString("%1×%2").arg(liveSequence()->width()).arg(liveSequence()->height()),
            track != nullptr ? QString::fromStdString(track->name()) : QString{});
    }

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
