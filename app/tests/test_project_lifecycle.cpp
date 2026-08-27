// The project itself: saving, versions, locks, delivery and nesting.
//
// Driven through the real window against the real compositor. See GuiFixture.h
// for what is shared and why.

#include <catch2/catch_test_macros.hpp>

#include "../AudioStrip.h"
#include "../FrameGrab.h"
#include "GuiFixture.h"

// The suite was written inside main(), which had this at file scope; the
// bodies still say `model::` and `Status` unqualified.
using namespace zaro;

using zaro::app::dragOnTimeline;
using zaro::app::PreviewWindow;
using zaro::app::settledGrab;

// Deliver: the panel actually renders a file.
//
// The export dialog is covered by the render tests; what this checks is
// the Deliver workspace's own path -- that its settings become a
// RenderRequest, that the queue runs it on its worker, and that a file
// arrives. A screen full of controls that produce nothing is the
// failure worth catching here.
TEST_CASE("The Deliver panel renders a file", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

    const std::filesystem::path deliverRoot =
        std::filesystem::temp_directory_path() / "zaro-selftest-deliver";
    std::filesystem::remove_all(deliverRoot);
    std::filesystem::create_directories(deliverRoot);

    app::DeliverPanel* deliver = window.deliver();
    window.setWorkspace("Deliver");
    QApplication::processEvents();
    deliver->setDestination(QString::fromStdString(deliverRoot.string()), "selftest");
    deliver->queueCurrent();
    if (deliver->queueLength() != 1) {
        zaro::app::testing::failf("adding to the queue added %d jobs\n", deliver->queueLength());
    }

    deliver->toggleRendering();
    // Rendering happens on the panel's own thread and reports back
    // through the event loop, so the test has to run one -- and has to
    // actually wait. `processEvents` with a time limit returns as soon
    // as the queue is empty rather than filling that time, so spinning
    // it four thousand times took milliseconds and declared a render
    // that had not started yet a failure.
    QElapsedTimer clock;
    clock.start();
    while (deliver->finishedCount() == 0 && clock.elapsed() < 180000) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    const std::filesystem::path written = deliverRoot / "selftest.mp4";
    if (deliver->finishedCount() != 1 || !std::filesystem::exists(written)) {
        zaro::app::testing::failf("the queue rendered nothing (%s)\n",
                                  deliver->lastMessage().toStdString().c_str());
    }
    const auto size = std::filesystem::file_size(written);
    if (size < 1024) {
        zaro::app::testing::failf("it wrote %llu bytes\n", static_cast<unsigned long long>(size));
    }
    std::printf("  deliver: queued one job and rendered %llu bytes to %s\n",
                static_cast<unsigned long long>(size), "selftest.mp4");

    std::filesystem::remove_all(deliverRoot);
    window.setWorkspace("Edit");
    QApplication::processEvents();
}

// Delivery: the curve a sequence goes out through, and the highlight
// rolloff that keeps the encoder from clipping.
//
// Driven through the same method the menu calls. What is checked is
// that it reaches the sequence, that the renderer reads it back, and
// that undo puts it where it was -- a delivery setting somebody cannot
// undo is one they cannot experiment with.
TEST_CASE("The delivery curve a sequence goes out through", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

    const auto deliverySequenceId = window.project().activeSequence();
    const auto wasOutput = window.project().findSequence(deliverySequenceId)->output();
    if (wasOutput.highlightKnee != 1.0) {
        zaro::app::testing::failf(
            "a project should start delivering as it always "
            "did, clipping its highlights\n");
    }

    zaro::model::Sequence::Output rolled;
    rolled.transfer = zaro::media::TransferFunction::SRGB;
    rolled.highlightKnee = 0.8;
    if (!window.setDelivery(rolled)) {
        zaro::app::testing::failf("the delivery could not be set\n");
    }
    const auto& delivered = window.project().findSequence(deliverySequenceId)->output();
    if (delivered.transfer != zaro::media::TransferFunction::SRGB ||
        delivered.highlightKnee != 0.8) {
        zaro::app::testing::failf("the delivery did not reach the sequence\n");
    }
    // And it still renders through it rather than refusing.
    window.monitor()->update();
    QApplication::processEvents();
    if (!window.monitor()->lastError().isEmpty()) {
        zaro::app::testing::failf("rendering reported %s\n",
                                  window.monitor()->lastError().toUtf8().constData());
    }

    // A curve with no formula is refused rather than accepted and left
    // to fail at the encoder.
    zaro::model::Sequence::Output nonsense;
    nonsense.transfer = zaro::media::TransferFunction::Unknown;
    if (zaro::edit::makeSetSequenceOutput(window.project(), deliverySequenceId, nonsense)) {
        zaro::app::testing::failf("a curve with no formula was accepted\n");
    }

    std::printf("  delivery: %s at knee %.2f, undo restores %s at %.2f\n",
                zaro::media::toString(delivered.transfer), delivered.highlightKnee,
                zaro::media::toString(wasOutput.transfer), wasOutput.highlightKnee);

    window.commands().undo(window.project());
    const auto& afterUndo = window.project().findSequence(deliverySequenceId)->output();
    if (afterUndo.transfer != wasOutput.transfer ||
        afterUndo.highlightKnee != wasOutput.highlightKnee) {
        zaro::app::testing::failf("undo did not restore the delivery\n");
    }
    window.monitor()->update();
    QApplication::processEvents();
}

// Saving, and the recovery file. Written to a scratch path rather than
// over the fixture: a self-test that rewrites its own input is one
// whose second run tests something else.
TEST_CASE("Saving, and the recovery file", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path() / "zaro-selftest-save";
    std::error_code code;
    std::filesystem::create_directories(scratch, code);
    const std::string savePath = (scratch / "project.zaro").string();
    std::filesystem::remove(savePath, code);
    std::filesystem::remove(zaro::io::autosavePath(savePath), code);

    window.setProjectPath(savePath);
    window.commands().markSaved();
    if (window.commands().isModified()) {
        zaro::app::testing::failf("a project just marked saved reads as modified\n");
    }

    const auto saveSequenceId = window.project().activeSequence();
    const auto saveTrackId =
        window.project().findSequence(saveSequenceId)->videoTracks().front().id();
    const auto saveRate = window.project().findSequence(saveSequenceId)->frameRate();
    auto marker = zaro::edit::makeAddMarker(window.project(), saveSequenceId,
                                            zaro::time::RationalTime{7, saveRate},
                                            zaro::time::RationalTime{1, saveRate}, "saved here");
    if (!marker) {
        zaro::app::testing::failf("%s\n", marker.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*marker));
    window.commands().breakMerge();
    if (!window.commands().isModified()) {
        zaro::app::testing::failf("an edit did not mark the project modified\n");
    }

    // Through the button, the way somebody would.
    auto* saveAction = window.findChild<QAction*>("save-project");
    if (saveAction == nullptr) {
        zaro::app::testing::failf("there is no Save item\n");
    }
    saveAction->trigger();
    QApplication::processEvents();

    if (!std::filesystem::exists(savePath)) {
        zaro::app::testing::failf("saving wrote no file\n");
    }
    if (window.commands().isModified()) {
        zaro::app::testing::failf("the project still reads as modified after a save\n");
    }
    if (window.windowTitle().contains('*')) {
        zaro::app::testing::failf("the title still says modified after a save\n");
    }

    // And what came back is the edit that was made.
    auto reloaded = zaro::io::loadProject(savePath);
    if (!reloaded) {
        zaro::app::testing::failf("%s\n", reloaded.error().toString().c_str());
    }
    const auto* savedSequence = reloaded->project.findSequence(saveSequenceId);
    if (savedSequence == nullptr || savedSequence->markers().empty()) {
        zaro::app::testing::failf("the saved file does not hold the edit\n");
    }
    static_cast<void>(saveTrackId);

    // A further edit, then the recovery file -- which must not touch
    // the project itself.
    const auto savedSize = std::filesystem::file_size(savePath);
    auto second = zaro::edit::makeAddMarker(window.project(), saveSequenceId,
                                            zaro::time::RationalTime{9, saveRate},
                                            zaro::time::RationalTime{1, saveRate}, "not saved yet");
    if (!second) {
        zaro::app::testing::failf("%s\n", second.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*second));
    window.commands().breakMerge();
    window.autosave();

    if (!zaro::io::hasNewerAutosave(savePath)) {
        zaro::app::testing::failf("autosaving left nothing to recover\n");
    }
    if (std::filesystem::file_size(savePath) != savedSize) {
        zaro::app::testing::failf("autosaving wrote into the project file\n");
    }

    // Saving properly clears it: what it described is now in the
    // project, and offering it on the next open would be alarming.
    saveAction->trigger();
    QApplication::processEvents();
    if (std::filesystem::exists(zaro::io::autosavePath(savePath))) {
        zaro::app::testing::failf("the recovery file outlived the save\n");
    }

    std::printf("  saving: %lld bytes, recovery written and cleared\n",
                static_cast<long long>(std::filesystem::file_size(savePath)));

    std::filesystem::remove_all(scratch, code);
    window.setProjectPath({});
    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
}

// Multicam, switched with the keyboard the way it is actually cut:
// watch it play and press the camera you want. The angles and the cut
// are tested headlessly; what that cannot show is whether the key
// reaches the edit.
TEST_CASE("Multicam, switched with the keyboard", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

    const auto outerSequenceId = sequence.id();
    const auto trackId = videoTrack.id();
    const auto mediaId = window.project().media().front().id;

    zaro::model::Clip::Angle a;
    a.media = mediaId;
    a.offset = zaro::time::RationalTime{0, sequence.frameRate()};
    a.name = "A";
    zaro::model::Clip::Angle b;
    b.media = mediaId;
    // A different point in the same file stands in for a second camera:
    // what is being checked is the switch, not the footage.
    b.offset = zaro::time::RationalTime{50, sequence.frameRate()};
    b.name = "B";

    auto built = zaro::edit::makeMulticam(
        window.project(), {outerSequenceId, trackId}, {a, b},
        zaro::time::TimeRange{zaro::time::RationalTime{0, sequence.frameRate()},
                              zaro::time::RationalTime{60, sequence.frameRate()}});
    if (!built) {
        zaro::app::testing::failf("%s\n", built.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*built));

    const auto clipsOn = [&]() {
        return window.project().findSequence(outerSequenceId)->findTrack(trackId)->clips().size();
    };
    const std::size_t clipsBeforeSwitch = clipsOn();

    // Select it, put the playhead inside it, and press 2.
    const auto id =
        window.project().findSequence(outerSequenceId)->findTrack(trackId)->clips().front().id;
    window.effects()->setSelection(trackId, id);
    timeline->selectOnly(trackId, id);
    window.setPosition(zaro::time::RationalTime{25, sequence.frameRate()});
    QApplication::processEvents();

    QKeyEvent two(QEvent::KeyPress, Qt::Key_2, Qt::NoModifier);
    QCoreApplication::sendEvent(timeline, &two);
    QApplication::processEvents();

    const std::size_t clipsAfterSwitch = clipsOn();
    std::printf("  multicam: %zu clips before the switch, %zu after\n", clipsBeforeSwitch,
                clipsAfterSwitch);
    if (clipsAfterSwitch != clipsBeforeSwitch + 1) {
        zaro::app::testing::failf("switching an angle did not cut the clip\n");
    }
    const auto& clips = window.project().findSequence(outerSequenceId)->findTrack(trackId)->clips();
    if (clips[1].activeAngle != 1 || clips[1].start().frames() != 25) {
        zaro::app::testing::failf("the cut is in the wrong place or on the wrong angle\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.monitor()->update();
    QApplication::processEvents();
}

// Second to last: this adds a track, and adding one reallocates the
// sequence's vector of them -- so the reference this function has
// held since the top must be finished with by now.
// Syncing multicam angles, through the real window.
//
// Timecode rather than by ear: the arithmetic of both is tested
// headlessly, and what this has to show is that picking a clip, asking
// for a sync and having the offsets land on it works end to end. By
// ear would also mean reading a minute of audio from a ten-second
// fixture, which is a test of silence handling, not of syncing.
TEST_CASE("Multicam sync across angles", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

    const auto syncSequenceId = window.project().activeSequence();
    const auto syncTrackId =
        window.project().findSequence(syncSequenceId)->videoTracks().front().id();
    const auto syncRate = window.project().findSequence(syncSequenceId)->frameRate();

    // A second reference to the same file stands in for a second
    // camera: what is being checked is the sync, not the footage. Two
    // references so that each can carry its own start timecode, which
    // is what a second camera would have.
    zaro::model::MediaRef cameraB = window.project().media().front();
    cameraB.id = window.project().ids().next<zaro::model::MediaRefTag>();
    cameraB.name = "cam-b";
    const auto cameraBId = window.project().addMedia(cameraB);

    const auto stamp = [&](zaro::model::MediaRefId id, const char* text) {
        for (zaro::model::MediaRef& media : window.project().mediaMutable()) {
            if (media.id == id && !media.info.videoStreams.empty()) {
                media.info.videoStreams.front().startTimecode = zaro::time::parseTimecode(text);
            }
        }
    };
    stamp(window.project().media().front().id, "01:00:00:00");
    // Rolled two seconds later on the same clock.
    stamp(cameraBId, "01:00:02:00");
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }

    zaro::model::Clip::Angle angleA;
    angleA.media = window.project().media().front().id;
    angleA.name = "A";
    zaro::model::Clip::Angle angleB;
    angleB.media = cameraBId;
    // Deliberately wrong to begin with, so that a sync which does
    // nothing at all cannot pass.
    angleB.offset = zaro::time::RationalTime{99, syncRate};
    angleB.name = "B";

    auto placed =
        zaro::edit::makeMulticam(window.project(), {syncSequenceId, syncTrackId}, {angleA, angleB},
                                 zaro::time::TimeRange{zaro::time::RationalTime{0, syncRate},
                                                       zaro::time::RationalTime{60, syncRate}});
    if (!placed) {
        zaro::app::testing::failf("%s\n", placed.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*placed));

    const auto syncClipId =
        window.project().findSequence(syncSequenceId)->findTrack(syncTrackId)->clips().front().id;
    timeline->selectOnly(syncTrackId, syncClipId);
    QApplication::processEvents();

    window.syncAngles(/*byEar=*/false);
    QApplication::processEvents();

    const auto* syncedClip =
        window.project().findSequence(syncSequenceId)->findTrack(syncTrackId)->find(syncClipId);
    if (syncedClip == nullptr) {
        zaro::app::testing::failf("the multicam clip went missing\n");
    }
    const double offsetSeconds = syncedClip->angles[1].offset.toSeconds().toDouble();
    std::printf("  multicam sync: %d angles synced, %d skipped, camera B at %+.2fs\n",
                window.lastSyncCount(), window.lastSyncSkipped(), offsetSeconds);
    if (window.lastSyncCount() != 2 || window.lastSyncSkipped() != 0) {
        zaro::app::testing::failf("the sync did not reach both angles\n");
    }
    // Two seconds later on the clock means reading two seconds *less*
    // far into that camera's own material for the same moment.
    if (std::abs(offsetSeconds + 2.0) > 0.05) {
        zaro::app::testing::failf("camera B was put at %+.2fs, not -2.00s\n", offsetSeconds);
    }

    // And it is one undoable step: the wrong offset comes back whole.
    window.commands().undo(window.project());
    const auto* undone =
        window.project().findSequence(syncSequenceId)->findTrack(syncTrackId)->find(syncClipId);
    if (undone == nullptr || undone->angles[1].offset.frames() != 99) {
        zaro::app::testing::failf("undoing the sync did not restore the offset\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    // Put the project back as it was found. Adding a media reference is
    // not a command, so nothing above undoes it, and the blocks that
    // follow are entitled to the project they were written against.
    window.project().mediaMutable().pop_back();
    window.project().mediaMutable().front().info.videoStreams.front().startTimecode = std::nullopt;
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    window.monitor()->update();
    QApplication::processEvents();
}

// Hotkeys: what the keys do, and changing it.
TEST_CASE("Hotkeys: what the keys do, and changing it", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

    auto* manager = window.showHotkeys();
    if (manager == nullptr) {
        zaro::app::testing::failf("there is no hotkey manager\n");
    }
    auto* saveAction = window.findChild<QAction*>("save-project");
    if (saveAction == nullptr) {
        zaro::app::testing::failf("commands are not addressable by name\n");
    }
    // Out of the box, the menu shows what the catalogue says.
    if (saveAction->shortcut() != QKeySequence("Ctrl+S")) {
        zaro::app::testing::failf("Save is bound to %s, not Ctrl+S\n",
                                  saveAction->shortcut().toString().toStdString().c_str());
    }

    // Rebind it, the way the manager does.
    if (Status bound = manager->assign("save-project", "Ctrl+Alt+9"); !bound) {
        zaro::app::testing::failf("%s\n", bound.error().toString().c_str());
    }
    if (saveAction->shortcut() != QKeySequence("Ctrl+Alt+9")) {
        zaro::app::testing::failf("rebinding did not reach the menu (%s)\n",
                                  saveAction->shortcut().toString().toStdString().c_str());
    }

    // A keystroke somebody else has is refused, and says whose it is.
    auto clash = manager->assign("relink-media", "Ctrl+Alt+9");
    if (clash) {
        zaro::app::testing::failf("two commands were allowed on one keystroke\n");
    }
    if (clash.error().message().find("Save") == std::string::npos) {
        zaro::app::testing::failf("the clash does not say what holds it: %s\n",
                                  clash.error().message().c_str());
    }

    // A command with no menu item is rebindable too, and the key
    // handler follows: mark-in moves from I to Y.
    //
    // What is checked is where the in point *is*, not whether there is
    // one: a marked range may already exist, so a test that asked "is
    // anything marked" would pass without either key doing anything --
    // which is exactly what it did until the revert check found it.
    // Loaded here rather than inherited. The comment below used to be right --
    // an earlier block left a clip in the source monitor -- and that is exactly
    // what made this test depend on running after one, which nothing guarantees
    // now that the sections are separate cases.
    window.sourceMonitor()->load(window.project().media().front());
    QApplication::processEvents();
    window.sourceMonitor()->step(7);
    const auto parkedAt = window.sourceMonitor()->position();
    if (Status moved = manager->assign("mark-in", "Y"); !moved) {
        zaro::app::testing::failf("%s\n", moved.error().toString().c_str());
    }
    QKeyEvent oldKey(QEvent::KeyPress, Qt::Key_I, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &oldKey);
    const auto afterOldKey = window.sourceMonitor()->markedRange();
    if (afterOldKey.has_value() && afterOldKey->start() == parkedAt) {
        zaro::app::testing::failf("the old key still marks in\n");
    }
    QKeyEvent newKey(QEvent::KeyPress, Qt::Key_Y, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &newKey);
    const auto afterNewKey = window.sourceMonitor()->markedRange();
    if (!afterNewKey.has_value() || afterNewKey->start() != parkedAt) {
        zaro::app::testing::failf("the new key did not mark in where the source is\n");
    }

    // It survives being written out and read back.
    const std::string written = window.keymap().encode();
    auto reloaded = zaro::ui::Keymap::decode(written);
    if (!reloaded || reloaded->shortcutFor("save-project") != "Ctrl+Alt+9" ||
        reloaded->shortcutFor("mark-in") != "Y") {
        zaro::app::testing::failf("the keymap did not survive a round trip\n");
    }
    std::printf("  hotkeys: %zu commands, %zu bytes of keymap when two are changed\n",
                zaro::ui::allActions().size(), written.size());

    // And putting them back is one call.
    if (Status back = manager->resetOne("save-project"); !back) {
        zaro::app::testing::failf("%s\n", back.error().toString().c_str());
    }
    if (saveAction->shortcut() != QKeySequence("Ctrl+S")) {
        zaro::app::testing::failf("resetting did not restore the default\n");
    }
    window.keymap().resetAll();
    window.applyKeymap();
    QApplication::processEvents();
}

// The render cache, through the real window.
//
// The cache only ever answers the CPU compositor, so this needs a
// sequence the preview cannot draw on the GPU -- which is exactly what
// an adjustment layer is. Rendered ahead, then played: what is checked
// is that the frames the monitor asks for come back from the cache
// rather than being composited again, and that an edit under them stops
// that happening.
TEST_CASE("The render cache, through the real window", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

    const auto cacheSequenceId = window.project().activeSequence();
    if (window.project().findSequence(cacheSequenceId)->videoTracks().size() < 2) {
        auto added = zaro::edit::makeAddTrack(window.project(), cacheSequenceId,
                                              zaro::model::TrackKind::Video, "V2");
        if (!added) {
            zaro::app::testing::failf("%s\n", added.error().toString().c_str());
        }
        window.commands().execute(window.project(), std::move(*added));
    }
    const auto cacheRate = window.project().findSequence(cacheSequenceId)->frameRate();
    const auto cacheTrackId = window.project().findSequence(cacheSequenceId)->videoTracks()[1].id();

    auto layer = zaro::edit::makeAddAdjustment(
        window.project(), {cacheSequenceId, cacheTrackId},
        zaro::time::TimeRange{zaro::time::RationalTime{0, cacheRate},
                              zaro::time::RationalTime{40, cacheRate}});
    if (!layer) {
        zaro::app::testing::failf("%s\n", layer.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*layer));
    const auto cacheLayerId =
        window.project().findSequence(cacheSequenceId)->findTrack(cacheTrackId)->clips().front().id;

    window.timeline()->zoomToFit();
    QApplication::processEvents();
    const auto covers = [](const std::vector<zaro::time::TimeRange>& spans, std::int64_t frame) {
        for (const zaro::time::TimeRange& span : spans) {
            if (frame >= span.start().frames() && frame < span.endExclusive().frames()) {
                return true;
            }
        }
        return false;
    };

    // Match frame left the source in the viewer, and a monitor that is
    // not on screen does not repaint -- so nothing would read the cache.
    window.showProgram();
    QApplication::processEvents();
    window.renderCache().clear();
    window.renderVisibleRange();
    const std::size_t cached = window.renderCache().count();
    const std::size_t spans = window.timeline()->cachedSpans().size();
    if (cached == 0 || spans == 0 || !covers(window.timeline()->cachedSpans(), 10)) {
        zaro::app::testing::failf("pre-rendering cached %zu frames in %zu spans\n", cached, spans);
    }

    // Play it. Every frame the monitor draws over this range should
    // already be in the cache.
    window.renderCache().resetStatistics();
    for (std::int64_t frame = 0; frame < 20; ++frame) {
        window.setPosition(zaro::time::RationalTime{frame, cacheRate});
        QApplication::processEvents();
    }
    const auto hits = window.renderCache().hits();
    const auto misses = window.renderCache().misses();
    std::printf(
        "  render cache: %zu frames pre-rendered, %llu hits and %llu misses "
        "playing them back\n",
        cached, static_cast<unsigned long long>(hits), static_cast<unsigned long long>(misses));
    if (hits == 0 || misses > hits) {
        zaro::app::testing::failf("the preview did not read from the render cache\n");
    }

    // And an edit under the bar takes it away. Not "eventually", and
    // not by anyone remembering to say so: the frame is made of
    // something that has changed, so it stops being a frame.
    zaro::model::ColorCorrection darker;
    darker.exposure = -2.0;
    auto graded = zaro::edit::makeSetColorCorrection(
        window.project(), {cacheSequenceId, cacheTrackId}, cacheLayerId, darker);
    if (!graded) {
        zaro::app::testing::failf("%s\n", graded.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*graded));
    // What the timeline's `edited` signal calls. Driven directly here
    // because this edit was made against the model rather than through
    // the widget that emits it.
    window.updateCacheBar();
    QApplication::processEvents();
    if (covers(window.timeline()->cachedSpans(), 10)) {
        zaro::app::testing::failf("the cache bar survived an edit under it\n");
    }
    // And only where the edit reaches: the layer stops at frame 40, so
    // the rest of the timeline is still rendered. A cache that threw
    // everything away on every edit would pass the check above and be
    // useless.
    if (!covers(window.timeline()->cachedSpans(), 200)) {
        zaro::app::testing::failf("an edit over 40 frames cleared the whole bar\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.renderCache().clear();
    window.monitor()->update();
    QApplication::processEvents();
}

// Last on purpose. Adding a sequence reallocates the project's
// vector of them, so the reference this function has held since the
// top dangles from here on -- and everything else is done by now.
// A nested sequence, through the real preview. The recursion and the
// cycle refusal are tested headlessly; what those cannot show is
// whether a nest reaches the screen, which on the GPU path means a CPU
// composite and an upload rather than the ordinary route.
TEST_CASE("A nested sequence renders through the parent", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

    // Captured by value first: adding a sequence reallocates the
    // project's vector, and the reference this self-test has been
    // holding since the top would dangle. It did, and the first run
    // aborted inside rational arithmetic reading the wreckage.
    const auto outerId = window.project().activeSequence();
    const auto outerRate = window.project().findSequence(outerId)->frameRate();
    const auto outerTrackId = window.project().findSequence(outerId)->videoTracks().front().id();

    // An inner sequence holding a white rectangle over its whole
    // length, so what it contributes is unmistakable.
    zaro::model::Sequence inner{window.project().ids().next<zaro::model::SequenceTag>(), "nested",
                                outerRate};
    inner.setSize(window.sequence()->width(), window.sequence()->height());
    const auto innerId = inner.id();
    const auto innerTrack = window.project().ids().next<zaro::model::TrackTag>();
    inner.addTrack(innerTrack, zaro::model::TrackKind::Video, "V1");
    window.project().addSequence(std::move(inner));
    window.rebindSequence();

    zaro::model::Graphic block;
    block.kind = zaro::model::GraphicKind::Rectangle;
    block.width = 4000.0;
    block.height = 4000.0;
    auto filled =
        zaro::edit::makeAddGraphic(window.project(), {innerId, innerTrack}, block,
                                   zaro::time::TimeRange{zaro::time::RationalTime{0, outerRate},
                                                         zaro::time::RationalTime{40, outerRate}});
    if (!filled) {
        zaro::app::testing::failf("%s\n", filled.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*filled));

    std::int64_t darkFrame = 0;
    double darkest = 1e9;
    for (std::int64_t frame = 0; frame < 35; ++frame) {
        window.setPosition(zaro::time::RationalTime{frame, outerRate});
        QApplication::processEvents();
        const double gray = meanGray(settledGrab(window.monitor()));
        if (gray < darkest) {
            darkest = gray;
            darkFrame = frame;
        }
    }
    window.setPosition(zaro::time::RationalTime{darkFrame, outerRate});
    QApplication::processEvents();
    const double without = meanGray(settledGrab(window.monitor()));

    auto nested = zaro::edit::makeNestSequence(window.project(), {outerId, outerTrackId}, innerId,
                                               zaro::time::RationalTime{0, outerRate});
    if (!nested) {
        zaro::app::testing::failf("%s\n", nested.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*nested));
    window.monitor()->update();
    QApplication::processEvents();
    const double withNest = meanGray(settledGrab(window.monitor()));

    std::printf("  nesting: %.1f with a nested sequence, %.1f without\n", withNest, without);
    if (!(withNest > without + 50.0)) {
        zaro::app::testing::failf("the nested sequence did not reach the preview\n");
    }

    // And a sequence still cannot contain itself.
    if (zaro::edit::makeNestSequence(window.project(), {outerId, outerTrackId}, outerId,
                                     zaro::time::RationalTime{0, outerRate})) {
        zaro::app::testing::failf("a sequence was allowed inside itself\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.monitor()->update();
    QApplication::processEvents();
}

// Review comments: leave notes, tick them off, send the list.
TEST_CASE("Review comments, ticked off and sent", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

    const auto reviewSequenceId = window.project().activeSequence();
    const auto reviewRate = window.project().findSequence(reviewSequenceId)->frameRate();

    struct Note {
        std::int64_t frame;
        const char* name;
        const char* text;
    };
    const Note notes[] = {
        {50, "Title", "spell the surname with two n's"},
        {12, "Music", "comes in too early"},
    };
    for (const Note& note : notes) {
        auto added = zaro::edit::makeAddMarker(window.project(), reviewSequenceId,
                                               zaro::time::RationalTime{note.frame, reviewRate},
                                               zaro::time::RationalTime{1, reviewRate}, note.name);
        if (!added) {
            zaro::app::testing::failf("%s\n", added.error().toString().c_str());
        }
        window.commands().execute(window.project(), std::move(*added));
        // Found by where it is, not by where it landed in the list:
        // markers are kept in time order, so the one just added is not
        // necessarily the last.
        zaro::model::MarkerId markerId;
        for (const auto& candidate : window.project().findSequence(reviewSequenceId)->markers()) {
            if (candidate.range.start().frames() == note.frame) {
                markerId = candidate.id;
            }
        }
        if (!markerId.isValid()) {
            zaro::app::testing::failf("the marker was not added\n");
        }
        auto said = zaro::edit::makeUpdateMarker(window.project(), reviewSequenceId, markerId,
                                                 note.name, note.text, 0);
        if (!said) {
            zaro::app::testing::failf("%s\n", said.error().toString().c_str());
        }
        window.commands().execute(window.project(), std::move(*said));
    }

    // Tick the one at frame 12 off, through the same action the menu
    // uses: the playhead is the selection.
    window.setPosition(zaro::time::RationalTime{12, reviewRate});
    QApplication::processEvents();
    auto ticked = window.toggleCommentHere();
    if (!ticked || !*ticked) {
        zaro::app::testing::failf("resolving the comment did not take\n");
    }
    // And again puts it back, because the mistake people make is
    // ticking off the wrong one.
    auto untocked = window.toggleCommentHere();
    if (!untocked || *untocked) {
        zaro::app::testing::failf("the resolve toggle does not toggle\n");
    }
    static_cast<void>(window.toggleCommentHere());

    const std::filesystem::path notesPath =
        std::filesystem::temp_directory_path() / "zaro-selftest-review.md";
    std::filesystem::remove(notesPath);
    if (Status written = window.writeReviewNotes(notesPath.string()); !written) {
        zaro::app::testing::failf("%s\n", written.error().toString().c_str());
    }
    std::ifstream reading{notesPath};
    const std::string text{std::istreambuf_iterator<char>{reading},
                           std::istreambuf_iterator<char>{}};
    std::printf("  review notes: %zu bytes, %s\n", text.size(),
                text.find("1 of 2 done") != std::string::npos ? "one of two done" : "count wrong");
    if (text.find("comes in too early") == std::string::npos ||
        text.find("spell the surname") == std::string::npos) {
        zaro::app::testing::failf("the notes are not in the list\n");
    }
    if (text.find("1 of 2 done") == std::string::npos) {
        zaro::app::testing::failf("the list does not say what is done\n");
    }
    // In picture order, whatever order they were written in.
    if (text.find("comes in too early") > text.find("spell the surname")) {
        zaro::app::testing::failf("the list is not in time order\n");
    }
    // And it survives a save and a reopen, since a review outlives a
    // session.
    const auto* firstMarker = &window.project().findSequence(reviewSequenceId)->markers().front();
    static_cast<void>(firstMarker);

    std::filesystem::remove(notesPath);
    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    QApplication::processEvents();
}

// Sharing: a lock beside the project, and what it stops.
//
// Turned on for this block only: locking is off by default because a
// lock left behind interrupts an ordinary run, and the feature it
// protects is worth testing anyway.
TEST_CASE("A lock beside the project, and what it stops", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

    PreviewWindow::setLockingEnabled(true);
    const std::filesystem::path lockRoot =
        std::filesystem::temp_directory_path() / "zaro-selftest-locks";
    std::filesystem::remove_all(lockRoot);
    std::filesystem::create_directories(lockRoot);
    const std::string shared = (lockRoot / "shared.zaro").string();

    window.setProjectPath(shared);
    if (!window.save()) {
        zaro::app::testing::failf("the project would not save\n");
    }
    // Opening it takes the lock.
    if (Status opened = window.openProject(shared); !opened) {
        zaro::app::testing::failf("%s\n", opened.error().toString().c_str());
    }
    if (!std::filesystem::exists(zaro::io::lockPath(shared))) {
        zaro::app::testing::failf("opening a project took no lock\n");
    }
    if (window.isReadOnly()) {
        zaro::app::testing::failf("our own project opened read-only\n");
    }

    // Now somebody else has it, from a machine we cannot ask about.
    zaro::io::ProjectLock theirs = zaro::io::thisProcess();
    theirs.user = "someone";
    theirs.host = "another-machine";
    // A process id that is not running here, so what keeps this lock
    // standing is the rule that a pid from another machine means
    // nothing -- not the accident of it matching a live process.
    theirs.pid = 99999999;
    if (Status written = zaro::io::writeLock(shared, theirs); !written) {
        zaro::app::testing::failf("%s\n", written.error().toString().c_str());
    }
    if (Status refused = window.openProject(shared); refused) {
        zaro::app::testing::failf("a project somebody else has open still opened\n");
    }
    if (Status looked = window.openProject(shared, PreviewWindow::Sharing::ReadOnly); !looked) {
        zaro::app::testing::failf("%s\n", looked.error().toString().c_str());
    }
    if (!window.isReadOnly() || window.heldBy().find("someone") == std::string::npos) {
        zaro::app::testing::failf("it did not open read-only (%s)\n", window.heldBy().c_str());
    }
    // And read-only means it: the other machine's file is not written
    // over, and the lock is still theirs.
    const auto beforeSave = std::filesystem::last_write_time(shared);
    static_cast<void>(window.save());
    if (std::filesystem::last_write_time(shared) != beforeSave) {
        zaro::app::testing::failf("a read-only project was saved over\n");
    }
    auto stillTheirs = zaro::io::readLock(shared);
    if (!stillTheirs || stillTheirs->user != "someone") {
        zaro::app::testing::failf("read-only took somebody else's lock\n");
    }

    // A stale lock -- their machine crashed -- is not in the way.
    zaro::io::ProjectLock dead = zaro::io::thisProcess();
    dead.pid = 99999999;
    if (Status written = zaro::io::writeLock(shared, dead); !written) {
        zaro::app::testing::failf("%s\n", written.error().toString().c_str());
    }
    if (Status opened = window.openProject(shared); !opened) {
        zaro::app::testing::failf("a stale lock blocked opening: %s\n",
                                  opened.error().toString().c_str());
    }
    if (window.isReadOnly()) {
        zaro::app::testing::failf("a stale lock made the project read-only\n");
    }
    std::printf(
        "  sharing: locked, refused, read-only, then taken back from a stale "
        "lock\n");

    window.releaseLock();
    std::filesystem::remove_all(lockRoot);
    PreviewWindow::setLockingEnabled(false);
}

// Versioning: save a new version and carry on in it.
TEST_CASE("Saving a new version and carrying on in it", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

    const std::filesystem::path versionRoot =
        std::filesystem::temp_directory_path() / "zaro-selftest-versions";
    std::filesystem::remove_all(versionRoot);
    std::filesystem::create_directories(versionRoot);
    const std::string first = (versionRoot / "cut.zaro").string();

    window.setProjectPath(first);
    if (!window.save()) {
        zaro::app::testing::failf("the project would not save\n");
    }

    auto second = window.saveNewVersion();
    if (!second) {
        zaro::app::testing::failf("%s\n", second.error().toString().c_str());
    }
    auto third = window.saveNewVersion();
    if (!third) {
        zaro::app::testing::failf("%s\n", third.error().toString().c_str());
    }
    std::printf("  versions: %s then %s\n",
                std::filesystem::path{*second}.filename().string().c_str(),
                std::filesystem::path{*third}.filename().string().c_str());

    if (std::filesystem::path{*second}.filename() != "cut_v002.zaro" ||
        std::filesystem::path{*third}.filename() != "cut_v003.zaro") {
        zaro::app::testing::failf("the versions are not numbered in order\n");
    }
    // Every earlier version still there: that is what versioning is.
    if (!std::filesystem::exists(first) || !std::filesystem::exists(*second)) {
        zaro::app::testing::failf("a new version overwrote an old one\n");
    }
    // And the window is working in the newest one, so the next save
    // does not go back into the version somebody drew a line under.
    if (window.projectPath() != *third) {
        zaro::app::testing::failf("the window is still in the old version\n");
    }
    if (window.commands().isModified()) {
        zaro::app::testing::failf("a freshly versioned project reads as modified\n");
    }
    const auto listed = zaro::io::versionsOf(*third);
    if (listed.size() != 3) {
        zaro::app::testing::failf("%zu versions listed, not three\n", listed.size());
    }
    // An older version still opens, which is the reason to keep them.
    if (Status back = window.openProject(first); !back) {
        zaro::app::testing::failf("%s\n", back.error().toString().c_str());
    }
    std::filesystem::remove_all(versionRoot);
}

// New and Open, through the real window.
//
// Last, and on purpose: it replaces the project this whole self-test
// has been editing, so nothing after it could rely on what came before.
TEST_CASE("New and Open, through the real window", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

    const std::string originalPath = "/tmp/play/linked.zaro";
    const std::size_t originalMedia = window.project().media().size();

    // Something to undo, so that "New cleared the history" is a check
    // that can fail: the block before this one drains the stack.
    //
    // The rate comes from the project rather than from the `sequence`
    // reference this function has held since the top: by here, blocks
    // above have added a track and a sequence, and both reallocate.
    const auto liveRate =
        window.project().findSequence(window.project().activeSequence())->frameRate();
    auto scribble = zaro::edit::makeAddMarker(window.project(), window.project().activeSequence(),
                                              zaro::time::RationalTime{3, liveRate},
                                              zaro::time::RationalTime{1, liveRate}, "before new");
    if (!scribble) {
        zaro::app::testing::failf("%s\n", scribble.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*scribble));
    if (!window.commands().canUndo()) {
        zaro::app::testing::failf("the setup for this check did not take\n");
    }

    auto* newAction = window.findChild<QAction*>("new-project");
    if (newAction == nullptr) {
        zaro::app::testing::failf("there is no New item\n");
    }
    newAction->trigger();
    QApplication::processEvents();

    const auto freshId = window.project().activeSequence();
    const auto* fresh = window.project().findSequence(freshId);
    if (fresh == nullptr || fresh->videoTracks().empty() || fresh->duration().frames() != 0) {
        zaro::app::testing::failf("New did not give an empty sequence with tracks\n");
    }
    if (!window.project().media().empty()) {
        zaro::app::testing::failf("New kept the old project's media\n");
    }
    if (window.commands().canUndo()) {
        zaro::app::testing::failf("New kept the old project's history\n");
    }
    window.monitor()->update();
    QApplication::processEvents();

    // An empty sequence takes the shape of the first thing put on it.
    zaro::media::VideoStreamInfo stream;
    stream.width = 4096;
    stream.height = 2160;
    stream.frameRate = zaro::time::rates::fps24;
    stream.duration = zaro::time::Rational{4, 1};
    auto conformed = zaro::edit::makeConformSequence(window.project(), freshId, stream.frameRate,
                                                     stream.width, stream.height);
    if (!conformed) {
        zaro::app::testing::failf("%s\n", conformed.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*conformed));
    const auto* shaped = window.project().findSequence(freshId);
    std::printf("  new project: sequence conformed to %dx%d\n", shaped->width(), shaped->height());
    if (shaped->width() != 4096 || shaped->frameRate() != zaro::time::rates::fps24) {
        zaro::app::testing::failf("the empty sequence did not take the media's shape\n");
    }

    // Once there is a cut on it, it will not change shape again.
    zaro::model::MediaRef placeholder;
    placeholder.id = window.project().ids().next<zaro::model::MediaRefTag>();
    placeholder.name = "placeholder";
    placeholder.info.duration = stream.duration;
    placeholder.info.videoStreams.push_back(stream);
    const auto placeholderId = window.project().addMedia(placeholder);

    zaro::model::Clip anchorClip;
    anchorClip.id = window.project().ids().next<zaro::model::ClipTag>();
    anchorClip.source = placeholderId;
    anchorClip.sourceRange = zaro::time::TimeRange{zaro::time::RationalTime{0, stream.frameRate},
                                                   zaro::time::RationalTime{24, stream.frameRate}};
    anchorClip.timelineRange = anchorClip.sourceRange;
    auto anchored = zaro::edit::makeOverwrite(
        window.project(), {freshId, shaped->videoTracks().front().id()}, anchorClip);
    if (!anchored) {
        zaro::app::testing::failf("%s\n", anchored.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*anchored));
    if (zaro::edit::makeConformSequence(window.project(), freshId, zaro::time::rates::fps50, 640,
                                        480)) {
        zaro::app::testing::failf("a sequence with a cut on it can still be retimed\n");
    }

    // Open: back to the project this self-test started from.
    if (Status back = window.openProject(originalPath); !back) {
        zaro::app::testing::failf("opening the original project failed\n");
    }
    if (window.project().media().size() != originalMedia) {
        zaro::app::testing::failf("opening did not restore the project's media\n");
    }
    if (window.commands().isModified()) {
        zaro::app::testing::failf("a freshly opened project reads as modified\n");
    }
    std::printf("  opened %zu media back from %s\n", window.project().media().size(),
                originalPath.c_str());

    // And a file that is not a project leaves the window as it was.
    const std::size_t mediaBeforeBadOpen = window.project().media().size();
    if (Status missing = window.openProject("/definitely/not/a/project.zaro"); missing) {
        zaro::app::testing::failf("opening a missing file reported success\n");
    }
    if (window.project().media().size() != mediaBeforeBadOpen) {
        zaro::app::testing::failf("a failed open half-replaced the window\n");
    }

    // Handed back, and checked: a self-test that left a lock beside
    // the fixture would make the next run look like somebody else had
    // the project open.
    PreviewWindow::setLockingEnabled(true);
    if (Status again = window.openProject(originalPath); !again) {
        zaro::app::testing::failf("%s\n", again.error().toString().c_str());
    }
    if (!std::filesystem::exists(zaro::io::lockPath(originalPath))) {
        zaro::app::testing::failf("opening with locking on took no lock\n");
    }
    window.releaseLock();
    PreviewWindow::setLockingEnabled(false);
    if (std::filesystem::exists(zaro::io::lockPath(originalPath))) {
        zaro::app::testing::failf("closing left a lock behind\n");
    }
}

// Switching sequence while a workspace other than Edit is up.
//
// Every panel holds which sequence it is about, so every panel has to be told
// when that changes. Which ones get told was a list written by hand in
// setActiveSequence, and it named four of the eleven: the rest were bound in
// setWorkspace instead, on the way into the workspace that shows them. That is
// the right place to bind them the first time and the wrong place to be the
// only place, because somebody already looking at the mixer when the sequence
// changes never leaves and comes back -- so the console stayed pointed at a
// sequence that was no longer the one being edited.
TEST_CASE("Every panel follows the sequence being edited", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;

    const auto firstId = window.project().activeSequence();
    const auto rate = window.project().findSequence(firstId)->frameRate();
    const auto firstAudioTrack = window.project().findSequence(firstId)->audioTracks().front().id();

    // A second sequence with an audio track of its own, so "which sequence is
    // the mixer showing" has two possible answers and they look different.
    zaro::model::Sequence second{window.project().ids().next<zaro::model::SequenceTag>(), "second",
                                 rate};
    second.setSize(window.sequence()->width(), window.sequence()->height());
    const auto secondId = second.id();
    const auto secondVideo = window.project().ids().next<zaro::model::TrackTag>();
    const auto secondAudio = window.project().ids().next<zaro::model::TrackTag>();
    second.addTrack(secondVideo, zaro::model::TrackKind::Video, "V1");
    second.addTrack(secondAudio, zaro::model::TrackKind::Audio, "A1");
    window.project().addSequence(std::move(second));
    window.rebindSequence();

    // Standing in the Audio workspace, the way somebody mixing would be.
    window.setWorkspace("Audio");
    QApplication::processEvents();

    const auto stripFor = [&](zaro::model::TrackId track) {
        return window.mixer()->findChild<app::AudioStrip*>(QString{"mixer-strip-"} +
                                                           QString::number(track.value()));
    };
    if (stripFor(firstAudioTrack) == nullptr) {
        zaro::app::testing::failf("the mixer is not showing the sequence being edited to start\n");
    }

    // The sequence changes underneath them.
    window.setActiveSequence(secondId);
    QApplication::processEvents();

    if (stripFor(secondAudio) == nullptr) {
        zaro::app::testing::failf(
            "the mixer did not follow the sequence: no strip for the new sequence's A1\n");
    }
    if (stripFor(firstAudioTrack) != nullptr) {
        zaro::app::testing::failf("the mixer is still showing the sequence that was left behind\n");
    }
    std::printf("  panels followed the sequence: mixer now on A1 of \"second\"\n");
}
