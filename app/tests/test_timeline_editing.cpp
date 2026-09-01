// Editing on the timeline: the blade, the mouse, and three-point assembly.
//
// Driven through the real window against the real compositor. See GuiFixture.h
// for what is shared and why.

#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"

#include "../FrameGrab.h"
#include "../MediaDrag.h"
#include "../ProjectBin.h"
#include "GuiFixture.h"

// The suite was written inside main(), which had this at file scope; the
// bodies still say `model::` and `Status` unqualified.
using namespace zaro;

using zaro::app::dragOnTimeline;
using zaro::app::settledGrab;

// Exercise the timeline's editing interactions the way a mouse does.
// The edit operations themselves are covered headlessly; what this
// checks is the wiring -- that a drag reaches the right operation with
// the right arguments, and that undo steps over the whole gesture.
// A drag acts on the clip under the pointer, not on whichever member of the
// selection happens to be first.
//
// The regression: pressing an edge anchored the trim to the pressed clip but
// applied the delta to the primary selection. With a second clip selected ahead
// of it, the trim went to the wrong clip -- and where the delta made no sense
// against that clip's edges the operation was refused, so the drag did nothing
// at all. It surfaced as an intermittent failure in the trim test above,
// because whether it fired depended on what selection the previous test left
// behind.
TEST_CASE("Dragging an edge of a selected clip acts on that clip", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    auto* timeline = window.timeline();
    const auto& sequence = *window.sequence();
    const auto& videoTrack = sequence.videoTracks().front();
    const auto& audioTrack = sequence.audioTracks().front();
    const auto target = videoTrack.clips().front();
    const auto other = audioTrack.clips().front();
    const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    const int y = row->top + row->height / 2;

    // Unlinked first, so the two outcomes are distinguishable: while the pair is
    // linked, trimming either one trims both, and a trim that went to the wrong
    // clip would still leave the right one looking correctly trimmed.
    {
        auto built = zaro::edit::makeUnlinkClips(window.project(), {sequence.id(), videoTrack.id()},
                                                 target.id);
        REQUIRE(built.hasValue());
        window.commands().execute(window.project(), std::move(*built));
        window.commands().breakMerge();
    }

    // Something else leads the selection, and the clip we are about to press is
    // in it but not first -- the state the previous test used to leave behind.
    timeline->selectOnly(audioTrack.id(), other.id);

    const int outX = static_cast<int>(timeline->layout().xForTime(target.endExclusive()));
    int pressX = 0;
    for (const int offset : {2, 3, 4, 5, 6}) {
        const auto hit = timeline->layout().hitTest(sequence, outX - offset, y);
        if (hit && hit->clip == target.id && hit->part == zaro::ui::TimelineLayout::Part::OutEdge) {
            pressX = outX - offset;
            break;
        }
    }
    if (pressX == 0) {
        zaro::app::testing::failf("no out edge to grab within 6px of x=%d on row y=%d\n", outX, y);
    }
    // Shift-click adds it to the set without making it the primary.
    QMouseEvent add(QEvent::MouseButtonPress, QPointF(pressX, y), QPointF(pressX, y),
                    Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
    QCoreApplication::sendEvent(timeline, &add);

    dragOnTimeline(timeline, pressX, pressX - 120, y);

    const auto* seqNow = window.project().findSequence(sequence.id());
    const zaro::model::Clip* trimmed = seqNow->videoTracks().front().find(target.id);
    if (trimmed == nullptr) {
        zaro::app::testing::failf("the clip disappeared\n");
    }
    const zaro::model::Clip* untouched = seqNow->audioTracks().front().find(other.id);
    const std::int64_t shortened = target.duration().frames() - trimmed->duration().frames();
    std::printf("  pressed clip %lld -> %lld, the one leading the selection %lld -> %lld\n",
                static_cast<long long>(target.duration().frames()),
                static_cast<long long>(trimmed->duration().frames()),
                static_cast<long long>(other.duration().frames()),
                static_cast<long long>(untouched->duration().frames()));
    if (shortened <= 0) {
        zaro::app::testing::failf(
            "dragging the pressed clip's out edge did not trim it: it is still %lld frames\n",
            static_cast<long long>(trimmed->duration().frames()));
    }
    // The other half of the same bug: the trim must not have landed on the clip
    // that merely happened to lead the selection.
    if (untouched->duration() != other.duration()) {
        zaro::app::testing::failf(
            "the trim landed on the wrong clip: the one leading the selection went from %lld to "
            "%lld frames\n",
            static_cast<long long>(other.duration().frames()),
            static_cast<long long>(untouched->duration().frames()));
    }
}

TEST_CASE("Trimming a clip's out point with the mouse, and undoing it", "[gui]") {
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

    // Trim the out point inwards by dragging its right edge left.
    //
    // Where to press is asked of the layout rather than assumed. The out edge
    // is a few pixels wide and the clip ends one pixel before xForTime says the
    // exclusive end is, so a fixed offset either lands on the edge or a pixel
    // past the end of the clip depending on how wide the window came up -- and
    // pressing past the end is not a trim that failed, it is a press on empty
    // track. Failing with the reason beats failing with "it did not shorten".
    const int outX = static_cast<int>(timeline->layout().xForTime(original.endExclusive()));
    int pressX = 0;
    for (const int offset : {2, 3, 4, 5, 6}) {
        const auto hit = timeline->layout().hitTest(sequence, outX - offset, y);
        if (hit && hit->clip == original.id &&
            hit->part == zaro::ui::TimelineLayout::Part::OutEdge) {
            pressX = outX - offset;
            break;
        }
    }
    if (pressX == 0) {
        zaro::app::testing::failf("no out edge to grab within 6px of x=%d on row y=%d\n", outX, y);
    }
    const int wantedX = pressX - 120;
    dragOnTimeline(timeline, pressX, wantedX, y);
    const zaro::model::Clip* trimmed =
        window.project().findSequence(sequence.id())->videoTracks().front().find(original.id);
    if (trimmed == nullptr) {
        zaro::app::testing::failf("the clip disappeared\n");
    }
    const std::int64_t shortened = original.duration().frames() - trimmed->duration().frames();
    std::printf("  after trimming out by 120px: %lld frames shorter\n",
                static_cast<long long>(shortened));
    if (shortened <= 0) {
        // With the geometry, because without it this says only that a drag did
        // nothing. What it has been every time so far is the press landing on a
        // layout the widget had already moved on from.
        zaro::app::testing::failf(
            "the trim did not shorten the clip: pressed x=%d (out edge at %d), dragged to %d, "
            "on a timeline %d wide; the clip is still %lld frames\n",
            pressX, outX, wantedX, timeline->width(),
            static_cast<long long>(trimmed->duration().frames()));
    }
    // The clip's start must not have moved: that is what distinguishes a
    // trim from a move.
    if (trimmed->start() != original.start()) {
        zaro::app::testing::failf("trimming the out point moved the clip\n");
    }

    // One undo, for the whole drag.
    const std::size_t depthBefore = window.commands().depth();
    window.commands().undo(window.project());
    const zaro::model::Clip* restored =
        window.project().findSequence(sequence.id())->videoTracks().front().find(original.id);
    if (restored == nullptr || restored->duration() != original.duration()) {
        zaro::app::testing::failf("one undo did not restore the clip\n");
    }
    std::printf("  one undo restored it (%zu command%s on the stack)\n", depthBefore,
                depthBefore == 1 ? "" : "s");
    if (depthBefore != 1) {
        zaro::app::testing::failf("the drag left %zu undo steps; it should coalesce to one\n",
                                  depthBefore);
    }
}

// Linked cutting: picture and sound are one edit. The operation is
// unit-tested; what this checks is that the blade in the window
// reaches it, since a razor that cuts only the track under the pointer
// is the one edit whose damage shows up much later.
TEST_CASE("A razor cuts picture and sound together", "[gui]") {
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

    const auto& rate = sequence.frameRate();
    const auto& videoStart = sequence.videoTracks().front();
    if (videoStart.clips().empty() || sequence.audioTracks().empty() ||
        !videoStart.clips().front().link.isValid()) {
        zaro::app::testing::failf("this project has no linked pair to cut\n");
    }
    const zaro::model::TrackId audioId = sequence.audioTracks().front().id();
    const zaro::time::RationalTime cutAt{90, rate};

    timeline->setTool(app::TimelineWidget::Tool::Blade);
    // The widget re-lays out when the tool changes, and the press below is
    // aimed at a pixel this layout computes. Without a turn of the event loop
    // in between, the blade sometimes landed on a layout that was still the old
    // one and cut nothing.
    QApplication::processEvents();
    const int cutX = static_cast<int>(timeline->layout().xForTime(cutAt));
    dragOnTimeline(timeline, cutX, cutX, y);
    timeline->setTool(app::TimelineWidget::Tool::Select);

    const auto clipsOn = [&](zaro::model::TrackId track) {
        return window.project().findSequence(sequence.id())->findTrack(track)->clips();
    };
    const auto& videoNow = clipsOn(videoStart.id());
    const auto& audioNow = clipsOn(audioId);
    if (videoNow.size() < 2) {
        zaro::app::testing::failf("the blade did not cut the track it was on\n");
    }
    // Where it actually landed, rather than where it was aimed: a
    // press is at a pixel, and a pixel is a frame or so wide.
    const zaro::time::RationalTime landed = videoNow[1].start();
    if ((landed - cutAt).abs().frames() > 2) {
        zaro::app::testing::failf("the cut landed %lld frames from the pointer\n",
                                  static_cast<long long>((landed - cutAt).abs().frames()));
    }
    if (audioNow.size() < 2 || audioNow[1].start() != landed) {
        zaro::app::testing::failf("cutting linked picture left its sound joined\n");
    }

    // And the halves are two pairs rather than one group of four, so
    // dragging one half does not take the other half's sound with it.
    if (videoNow[0].link != audioNow[0].link || videoNow[1].link != audioNow[1].link ||
        videoNow[0].link == videoNow[1].link) {
        zaro::app::testing::failf("the halves of a linked cut are not two link groups\n");
    }
    std::printf("  linked cut: one press cut V1 and A1 at %lld, leaving two pairs\n",
                static_cast<long long>(landed.frames()));

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    QApplication::processEvents();
}

// Alignment: where a cut lands when there is something to line it up
// with. The engine's snapping is unit-tested; what this checks is that
// the blade asks for it -- a cut that misses the edit point it was
// obviously aimed at is a frame of black nobody sees until export.
TEST_CASE("A cut snaps to the edit point it was aimed at", "[gui]") {
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

    if (sequence.audioTracks().empty() || sequence.audioTracks().front().clips().empty()) {
        zaro::app::testing::failf("no audio track to align against\n");
    }
    const auto& rate = sequence.frameRate();
    const zaro::model::TrackId audioTrackId = sequence.audioTracks().front().id();

    // Unlinked first, and deliberately: with picture and sound linked,
    // cutting the audio cuts the video at the same instant, and the
    // blade would then be aiming at an edit point its own track
    // already had -- which would pass without any snapping at all.
    if (auto unlink = zaro::edit::makeUnlinkClips(
            window.project(), {sequence.id(), sequence.videoTracks().front().id()},
            sequence.videoTracks().front().clips().front().id)) {
        window.commands().execute(window.project(), std::move(*unlink));
        window.commands().breakMerge();
    }

    // An edit point on another track, a long way from anything else.
    const zaro::time::RationalTime alignAt{120, rate};
    auto cutAudio = zaro::edit::makeRazor(window.project(), {sequence.id(), audioTrackId}, alignAt);
    if (!cutAudio) {
        zaro::app::testing::failf("%s\n", cutAudio.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*cutAudio));
    window.commands().breakMerge();

    const auto boundaryExists = [&](const zaro::time::RationalTime& at) {
        const auto& clips =
            window.project().findSequence(sequence.id())->videoTracks().front().clips();
        return std::any_of(clips.begin(), clips.end(),
                           [&](const zaro::model::Clip& clip) { return clip.start() == at; });
    };

    timeline->setTool(app::TimelineWidget::Tool::Blade);
    QApplication::processEvents();
    const int alignX = static_cast<int>(timeline->layout().xForTime(alignAt));
    const int aimedX = alignX + 8;
    // The test is only worth anything if the pointer is aiming at a
    // different frame from the one it should land on.
    const auto unsnapped = timeline->layout().timeForX(aimedX, rate);
    if (unsnapped == alignAt) {
        zaro::app::testing::failf(
            "eight pixels is under a frame at this zoom; the "
            "alignment check would pass without snapping\n");
    }
    dragOnTimeline(timeline, aimedX, aimedX, y);
    if (!boundaryExists(alignAt)) {
        zaro::app::testing::failf(
            "a cut aimed %lld frames from an edit point on another "
            "track did not snap to it\n",
            static_cast<long long>((unsnapped - alignAt).abs().frames()));
    }

    // And the playhead, which is the other thing a cut is usually
    // aimed at: somebody parks it on the frame they want and cuts.
    const zaro::time::RationalTime parkAt{180, rate};
    window.setPosition(parkAt);
    QApplication::processEvents();
    const int parkX = static_cast<int>(timeline->layout().xForTime(parkAt));
    dragOnTimeline(timeline, parkX - 7, parkX - 7, y);
    if (!boundaryExists(parkAt)) {
        zaro::app::testing::failf("a cut aimed near the playhead did not snap to it\n");
    }

    // Dragging a clip onto an edit point puts a guide up, and the
    // guide comes down when the gesture does: a line left on the
    // timeline afterwards is one that means nothing.
    {
        const auto& videoNow = window.project().findSequence(sequence.id())->videoTracks().front();
        const zaro::model::Clip& first = videoNow.clips().front();
        const int grabX = static_cast<int>(timeline->layout().xForTime(first.start())) + 30;
        const auto sendMouse = [&](QEvent::Type type, int mx, Qt::MouseButton button,
                                   Qt::MouseButtons buttons) {
            QMouseEvent event(type, QPointF(mx, y), QPointF(mx, y), button, buttons,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(timeline, &event);
        };
        // Aim the clip's start a few pixels off the audio cut, so the
        // snap has something to do and something to say.
        const int landOn = static_cast<int>(timeline->layout().xForTime(alignAt));
        sendMouse(QEvent::MouseButtonPress, grabX, Qt::LeftButton, Qt::LeftButton);
        for (int step = 1; step <= 6; ++step) {
            sendMouse(QEvent::MouseMove, grabX + (landOn + 6 - grabX) * step / 6, Qt::NoButton,
                      Qt::LeftButton);
        }
        const bool guideUp = timeline->showingSnapGuide();
        const auto guideAt = timeline->snapGuideTime();
        sendMouse(QEvent::MouseButtonRelease, landOn + 6, Qt::NoButton, Qt::NoButton);

        if (!guideUp || guideAt != alignAt) {
            zaro::app::testing::failf("dragging a clip onto an edit point put up no guide\n");
        }
        if (timeline->showingSnapGuide()) {
            zaro::app::testing::failf("the alignment guide outlived the drag\n");
        }
        while (window.commands().canUndo() &&
               window.project()
                       .findSequence(sequence.id())
                       ->videoTracks()
                       .front()
                       .clips()
                       .front()
                       .start() != zaro::time::RationalTime{0, rate}) {
            window.commands().undo(window.project());
        }
    }

    // Snapping off means off: the same gesture then lands where the
    // pointer actually was.
    timeline->setSnapEnabled(false);
    const zaro::time::RationalTime looseAim = timeline->layout().timeForX(alignX + 8, rate);
    dragOnTimeline(timeline, alignX + 8, alignX + 8, y);
    if (!boundaryExists(looseAim)) {
        zaro::app::testing::failf("with snapping off the cut still moved\n");
    }
    timeline->setSnapEnabled(true);

    std::printf(
        "  alignment: a cut %lld frames off landed on the edit point, one near "
        "the playhead landed on it, a drag put a guide up and took it down, "
        "and nothing moved with snapping off\n",
        static_cast<long long>((unsnapped - alignAt).abs().frames()));

    timeline->setTool(app::TimelineWidget::Tool::Select);
    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    QApplication::processEvents();
}

// Three-point editing, through the source monitor rather than by
// calling the operation directly: mark a range, put the playhead
// somewhere, and press the key.
TEST_CASE("Three-point editing through the source monitor", "[gui]") {
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

    const zaro::model::MediaRef& firstMedia = window.project().media().front();
    window.sourceMonitor()->load(firstMedia);
    window.sourceMonitor()->step(24);
    window.sourceMonitor()->markIn();
    window.sourceMonitor()->step(48);
    window.sourceMonitor()->markOut();
    QApplication::processEvents();

    const auto marked = window.sourceMonitor()->markedRange();
    if (!marked) {
        zaro::app::testing::failf("marking in and out produced no range\n");
    }
    std::printf("  marked %lld source frames\n",
                static_cast<long long>(marked->duration().frames()));

    const auto& targetTrack = window.project().findSequence(sequence.id())->videoTracks().front();
    const std::size_t clipsBefore = targetTrack.clips().size();

    window.setPosition(zaro::time::RationalTime{5000, sequence.frameRate()});
    auto placed = zaro::edit::makePlaceFromSource(
        window.project(), {sequence.id(), targetTrack.id()}, window.sourceMonitor()->media(),
        *marked, zaro::time::RationalTime{5000, sequence.frameRate()},
        zaro::edit::PlaceMode::Overwrite);
    if (!placed) {
        zaro::app::testing::failf("%s\n", placed.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*placed));

    const auto& trackAfter = window.project().findSequence(sequence.id())->videoTracks().front();
    if (trackAfter.clips().size() != clipsBefore + 1) {
        zaro::app::testing::failf("the three-point edit added no clip\n");
    }
    const zaro::model::Clip* placedClip =
        trackAfter.clipAt(zaro::time::RationalTime{5000, sequence.frameRate()});
    if (placedClip == nullptr) {
        zaro::app::testing::failf("nothing landed at the playhead\n");
    }
    std::printf("  placed %lld frames at the playhead\n",
                static_cast<long long>(placedClip->duration().frames()));
    if (placedClip->duration().frames() <= 0) {
        zaro::app::testing::failf("the placed clip has no duration\n");
    }
}

// Multi-selection, driven as a rubber band. Starting below the last
// track means the press lands on empty timeline rather than grabbing a
// clip, which is what makes it a band rather than a move.
TEST_CASE("Multi-selection, driven as a rubber band", "[gui]") {
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

    const auto* seq = window.project().findSequence(sequence.id());
    const auto lastRow = timeline->rowFor(seq->audioTracks().back().id());
    if (!lastRow) {
        zaro::app::testing::failf("no audio row\n");
    }
    const int belowTracks = lastRow->top + lastRow->height + 12;
    const int acrossTop = timeline->rowFor(seq->videoTracks().front().id())->top + 4;

    std::int64_t clipsBefore = 0;
    for (const auto* list : {&seq->videoTracks(), &seq->audioTracks()}) {
        for (const auto& track : *list) {
            clipsBefore += static_cast<std::int64_t>(track.clips().size());
        }
    }

    {
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(200, belowTracks),
                          QPointF(200, belowTracks), Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(timeline, &press);
        // Baseline after the press, not before: the press clears the
        // previous selection, and those clips repaint. Taking it here
        // leaves the band as the only difference.
        const QImage quiet = timeline->grab().toImage();
        for (int i = 1; i <= 8; ++i) {
            const int bx = 200 + (1400 - 200) * i / 8;
            const int by = belowTracks + (acrossTop - belowTracks) * i / 8;
            QMouseEvent move(QEvent::MouseMove, QPointF(bx, by), QPointF(bx, by), Qt::NoButton,
                             Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(timeline, &move);
        }
        // The band rectangle is the only feedback this gesture gives,
        // so check it actually reaches the screen, and only inside its
        // own bounds.
        const QImage banded = timeline->grab().toImage();
        // grab() returns device pixels, so on a retina display the
        // image is twice the size of the coordinates the events used.
        const auto dpr = static_cast<int>(banded.devicePixelRatio());
        const QRect drawn = QRect(QPoint(200, belowTracks), QPoint(1400, acrossTop))
                                .normalized()
                                .intersected(timeline->rect());
        std::int64_t changedInside = 0;
        std::int64_t changedOutside = 0;
        for (int py = 0; py < banded.height(); ++py) {
            for (int px = 0; px < banded.width(); ++px) {
                if (banded.pixel(px, py) == quiet.pixel(px, py)) {
                    continue;
                }
                if (drawn.adjusted(-2, -2, 2, 2).contains(px / dpr, py / dpr)) {
                    ++changedInside;
                } else {
                    ++changedOutside;
                }
            }
        }
        std::printf("  band drawn: %lld pixels changed inside, %lld outside\n",
                    static_cast<long long>(changedInside), static_cast<long long>(changedOutside));
        if (changedInside == 0) {
            zaro::app::testing::failf("the rubber band painted nothing\n");
        }
        if (changedOutside > 0) {
            zaro::app::testing::failf("the band painted outside its own rectangle\n");
        }

        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(1400, acrossTop),
                            QPointF(1400, acrossTop), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(timeline, &release);
    }

    // Dragging any member of a selection has to move all of it, by the
    // same amount. Record where everything sits, nudge one clip, and
    // compare the shifts.
    std::vector<std::pair<model::ClipId, std::int64_t>> startsBefore;
    for (const auto* list : {&seq->videoTracks(), &seq->audioTracks()}) {
        for (const auto& track : *list) {
            for (const auto& clip : track.clips()) {
                startsBefore.emplace_back(clip.id, clip.start().frames());
            }
        }
    }
    const auto videoRow = timeline->rowFor(seq->videoTracks().front().id());
    const int grabY = videoRow->top + videoRow->height / 2;
    dragOnTimeline(timeline, 400, 520, grabY);

    const auto* moved = window.project().findSequence(sequence.id());
    std::int64_t shifted = 0;
    std::int64_t commonShift = 0;
    for (const auto* list : {&moved->videoTracks(), &moved->audioTracks()}) {
        for (const auto& track : *list) {
            for (const auto& clip : track.clips()) {
                for (const auto& [id, was] : startsBefore) {
                    if (id != clip.id) {
                        continue;
                    }
                    const std::int64_t delta = clip.start().frames() - was;
                    if (delta == 0) {
                        continue;
                    }
                    ++shifted;
                    if (commonShift == 0) {
                        commonShift = delta;
                    } else if (commonShift != delta) {
                        zaro::app::testing::failf("selection moved unevenly (%lld vs %lld)\n",
                                                  static_cast<long long>(commonShift),
                                                  static_cast<long long>(delta));
                        shifted = -1;
                    }
                }
            }
        }
    }
    if (shifted < 2) {
        zaro::app::testing::failf("dragging one selected clip moved %lld clips\n",
                                  static_cast<long long>(shifted));
    }
    std::printf("  dragging one member moved %lld clips by %lld frames each\n",
                static_cast<long long>(shifted), static_cast<long long>(commonShift));
    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }

    // Deleting reports how much the band caught.
    QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QCoreApplication::sendEvent(timeline, &del);

    const auto* banded = window.project().findSequence(sequence.id());
    std::int64_t clipsAfter = 0;
    for (const auto* list : {&banded->videoTracks(), &banded->audioTracks()}) {
        for (const auto& track : *list) {
            clipsAfter += static_cast<std::int64_t>(track.clips().size());
        }
    }
    std::printf("  rubber band removed %lld of %lld clips\n",
                static_cast<long long>(clipsBefore - clipsAfter),
                static_cast<long long>(clipsBefore));
    if (clipsBefore - clipsAfter < 2) {
        zaro::app::testing::failf("the band selected fewer than two clips across tracks\n");
    }

    window.commands().undo(window.project());
}

TEST_CASE("Renaming a track from its header, and undoing it", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    auto* timeline = window.timeline();
    const auto& sequence = *window.sequence();
    const auto trackId = sequence.videoTracks().front().id();
    const std::string before = sequence.videoTracks().front().name();

    const auto row = timeline->rowFor(trackId);
    REQUIRE(row.has_value());
    // The name band sits after the V1 badge and before the three buttons on the
    // right. Aimed at the middle of the header rather than at the text, because
    // a track with no name still has somewhere to double-click.
    const int x = 60;
    const int y = row->top + row->height / 2;

    const QPointF at(x, y);
    QMouseEvent press(QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(timeline, &press);
    QMouseEvent twice(QEvent::MouseButtonDblClick, at, at, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(timeline, &twice);

    auto* editor = timeline->findChild<QLineEdit*>();
    if (editor == nullptr || !editor->isVisible()) {
        zaro::app::testing::failf("double-clicking a track header opened no name editor\n");
    }

    editor->setText("Dialogue");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(editor, &enter);

    const model::Track* renamed = window.sequence()->findTrack(trackId);
    REQUIRE(renamed != nullptr);
    if (renamed->name() != "Dialogue") {
        zaro::app::testing::failf("the track is called %s, not the name that was typed\n",
                                  renamed->name().c_str());
    }

    // A rename is an edit like any other, so it comes back off the stack.
    window.commands().undo(window.project());
    const model::Track* restored = window.sequence()->findTrack(trackId);
    REQUIRE(restored != nullptr);
    if (restored->name() != before) {
        zaro::app::testing::failf("undo left the track called %s, not %s\n",
                                  restored->name().c_str(), before.c_str());
    }
}

TEST_CASE("Escape abandons a track rename", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    auto* timeline = window.timeline();
    const auto trackId = window.sequence()->videoTracks().front().id();
    const std::string before = window.sequence()->videoTracks().front().name();

    const auto row = timeline->rowFor(trackId);
    REQUIRE(row.has_value());
    const QPointF at(60, row->top + row->height / 2);
    QMouseEvent press(QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(timeline, &press);
    QMouseEvent twice(QEvent::MouseButtonDblClick, at, at, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(timeline, &twice);

    auto* editor = timeline->findChild<QLineEdit*>();
    REQUIRE(editor != nullptr);
    editor->setText("Not this");
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(editor, &escape);

    const model::Track* track = window.sequence()->findTrack(trackId);
    REQUIRE(track != nullptr);
    if (track->name() != before) {
        zaro::app::testing::failf("escape still renamed the track to %s\n", track->name().c_str());
    }
}

TEST_CASE("Dragging a track boundary resizes that kind of track", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    auto* timeline = window.timeline();
    const auto trackId = window.sequence()->videoTracks().front().id();

    const auto before = timeline->rowFor(trackId);
    REQUIRE(before.has_value());
    const int startHeight = before->height;

    // Grab the row's bottom edge, in the header column.
    const int edgeY = before->top + before->height;
    const QPointF from(60, edgeY);
    const QPointF to(60, edgeY + 20);
    QMouseEvent press(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(timeline, &press);
    QMouseEvent move(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(timeline, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(timeline, &release);

    const int grown = timeline->trackHeight(zaro::model::TrackKind::Video);
    if (grown <= startHeight) {
        zaro::app::testing::failf("dragging the boundary down left video tracks at %d (was %d)\n",
                                  grown, startHeight);
    }
    // Audio is a separate setting and should not have moved with it.
    if (timeline->trackHeight(zaro::model::TrackKind::Audio) !=
        zaro::app::TimelineWidget::kDefaultAudioTrackHeight) {
        zaro::app::testing::failf("resizing video also changed the audio track height\n");
    }

    // Every row has to stay on screen: there is no vertical scrolling, so a
    // height that pushed one off the bottom would make it unreachable.
    for (const auto& row : timeline->layout().rows(*window.sequence())) {
        if (row.top + row.height > timeline->height()) {
            zaro::app::testing::failf("a track ran past the bottom of the panel\n");
        }
    }

    timeline->setTrackHeight(zaro::model::TrackKind::Video,
                             zaro::app::TimelineWidget::kDefaultVideoTrackHeight);
}

// A file dragged out of the media pane and let go of on the timeline.
//
// The gesture is delivered as Qt delivers it -- a drag-enter to ask whether the
// panel will take it, then the drop -- because what this covers is the wiring:
// that the bin's drag says which file, that where it lands decides which row
// and when, that a drop over something already cut makes a row instead of
// overwriting it, and that the whole thing is one undo.
TEST_CASE("A file dragged from the media pane lands on the timeline", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    auto* timeline = window.timeline();
    auto* bin = window.bin();
    if (timeline == nullptr || bin == nullptr) {
        zaro::app::testing::failf("there is no timeline or no media pane\n");
    }

    // Snapping off for the duration: the assertions below are about where a
    // drop lands, and a start time pulled onto a nearby edit point would be
    // measuring the snap instead.
    const bool snapWas = timeline->snapEnabled();
    timeline->setSnapEnabled(false);

    // A sound-only file, which the fixture project does not have and which the
    // rule about picture rows is about.
    const int imported = bin->importPaths(
        {QString::fromStdString(zaro::app::testing::mediaFixture("tone_48k.wav"))});
    if (imported != 1) {
        zaro::app::testing::failf("the sound file did not import\n");
    }
    const zaro::model::MediaRefId sound = window.project().media().back().id;
    const zaro::model::MediaRefId picture = window.project().media().front().id;

    // The drag the bin actually starts, rather than one this test made up. The
    // events below go straight to the timeline, which skips the half of the
    // gesture that begins in the list -- so the list has to be draggable, and
    // what it hands over has to be what the timeline reads.
    auto* binList = bin->findChild<QListWidget*>("bin-list");
    if (binList == nullptr || !binList->dragEnabled()) {
        zaro::app::testing::failf("the bin's rows cannot be dragged out\n");
    }
    int mediaRow = -1;
    for (int row = 0; row < binList->count(); ++row) {
        if (binList->item(row)->data(Qt::UserRole).toULongLong() != 0) {
            mediaRow = row;
            break;
        }
    }
    if (mediaRow < 0) {
        zaro::app::testing::failf("no row in the bin stands for a file\n");
    }
    const std::unique_ptr<QMimeData> fromBin{
        binList->model()->mimeData({binList->model()->index(mediaRow, 0)})};
    const auto decoded = zaro::app::decodeMediaDrag(fromBin.get());
    if (!decoded ||
        decoded->media.value() != binList->item(mediaRow)->data(Qt::UserRole).toULongLong()) {
        zaro::app::testing::failf("the bin's drag does not say which file it is\n");
    }

    // Zoomed out, so there is empty timeline to the right of the fixture's one
    // clip to drop things into.
    timeline->zoomToFit();
    timeline->zoomBy(0.4);
    QApplication::processEvents();

    const auto drop = [timeline](zaro::model::MediaRefId media, int x, int y) {
        const std::unique_ptr<QMimeData> mime{
            zaro::app::encodeMediaDrag(zaro::app::MediaDrag{media, {}})};
        QDragEnterEvent entering{QPoint{x, y}, Qt::CopyAction, mime.get(), Qt::LeftButton,
                                 Qt::NoModifier};
        QCoreApplication::sendEvent(timeline, &entering);
        if (!entering.isAccepted()) {
            zaro::app::testing::failf("the timeline refused a file dragged over it at %d,%d\n", x,
                                      y);
        }
        QDropEvent dropping{QPointF{static_cast<double>(x), static_cast<double>(y)}, Qt::CopyAction,
                            mime.get(), Qt::LeftButton, Qt::NoModifier};
        QCoreApplication::sendEvent(timeline, &dropping);
        QApplication::processEvents();
    };

    // Looked up afresh every time: each drop executes a command, which replaces
    // the sequence, so anything held across one is a reference into freed
    // memory. This is the bug the fixture's own comment warns about.
    const auto videoRowY = [&window, timeline](std::size_t index) {
        const auto row = window.sequence()->videoTracks()[index].id();
        const auto found = timeline->rowFor(row);
        REQUIRE(found.has_value());
        return found->top + found->height / 2;
    };
    const auto audioRowY = [&window, timeline](std::size_t index) {
        const auto row = window.sequence()->audioTracks()[index].id();
        const auto found = timeline->rowFor(row);
        REQUIRE(found.has_value());
        return found->top + found->height / 2;
    };

    const zaro::time::Rational rate = window.sequence()->frameRate();
    const auto cutEnd = window.sequence()->videoTracks().front().extent().endExclusive();
    const int overX = static_cast<int>(timeline->layout().xForTime(
        cutEnd - zaro::time::RationalTime::fromSeconds(zaro::time::Rational{1, 2}, rate)));
    const int emptyX = timeline->width() - 40;
    if (timeline->layout().timeForX(emptyX, rate) <= cutEnd) {
        zaro::app::testing::failf("the timeline is not zoomed out enough to have empty room\n");
    }

    const std::size_t stepsBefore = window.commands().position();
    const std::size_t videoTracksBefore = window.sequence()->videoTracks().size();
    const std::size_t audioTracksBefore = window.sequence()->audioTracks().size();
    const std::size_t v1ClipsBefore = window.sequence()->videoTracks().front().clips().size();
    const std::size_t a1ClipsBefore = window.sequence()->audioTracks().front().clips().size();

    // 1. Onto empty timeline on V1: it joins the row it was let go of on, and
    // its sound goes under it. Two clips, because that is what the program can
    // hear -- the audio graph mixes clips on audio tracks and nothing else.
    drop(picture, emptyX, videoRowY(0));
    if (window.sequence()->videoTracks().size() != videoTracksBefore) {
        zaro::app::testing::failf("a drop into empty room made a track it did not need\n");
    }
    if (window.sequence()->videoTracks().front().clips().size() != v1ClipsBefore + 1) {
        zaro::app::testing::failf("the drop did not land on V1\n");
    }
    const auto landed = window.sequence()->videoTracks().front().clips().back();
    if (landed.start() != timeline->layout().timeForX(emptyX, rate)) {
        zaro::app::testing::failf(
            "the clip starts at %lld, not where it was let go of (%lld)\n",
            static_cast<long long>(landed.start().frames()),
            static_cast<long long>(timeline->layout().timeForX(emptyX, rate).frames()));
    }
    if (window.sequence()->audioTracks().size() != audioTracksBefore ||
        window.sequence()->audioTracks().front().clips().size() != a1ClipsBefore + 1) {
        zaro::app::testing::failf("the take's sound did not land on A1\n");
    }
    const auto& landedSound = window.sequence()->audioTracks().front().clips().back();
    if (landedSound.start() != landed.start() || landedSound.duration() != landed.duration()) {
        zaro::app::testing::failf("the sound is not where the picture is\n");
    }
    // Linked, so a later drag of either takes the other with it.
    if (!landed.link.isValid() || landedSound.link != landed.link) {
        zaro::app::testing::failf("picture and sound arrived unlinked\n");
    }

    // 2. Over the clip already cut on V1: a new picture row rather than an
    // overwrite of somebody's cut -- and, since A1 is busy at that time too, a
    // new sound row for the sound.
    const std::size_t audioTracksAfterFirst = window.sequence()->audioTracks().size();
    drop(picture, overX, videoRowY(0));
    if (window.sequence()->videoTracks().size() != videoTracksBefore + 1) {
        zaro::app::testing::failf("dropping over an existing clip made %zu video tracks, not %zu\n",
                                  window.sequence()->videoTracks().size(), videoTracksBefore + 1);
    }
    if (window.sequence()->videoTracks().front().clips().size() != v1ClipsBefore + 1) {
        zaro::app::testing::failf("the drop overwrote what was already on V1\n");
    }
    if (window.sequence()->videoTracks().back().clips().size() != 1) {
        zaro::app::testing::failf("the new video track did not get the clip\n");
    }
    if (window.sequence()->audioTracks().size() != audioTracksAfterFirst + 1) {
        zaro::app::testing::failf("the sound overwrote A1 instead of taking a row of its own\n");
    }
    if (window.sequence()->audioTracks().back().clips().size() != 1) {
        zaro::app::testing::failf("the new sound row did not get the sound\n");
    }

    // 3. A sound file over the sound block, where A1 is already busy: a new
    // sound row, and no picture anywhere -- a file with no picture in it has
    // none to place.
    const std::size_t videoTracksAfterSecond = window.sequence()->videoTracks().size();
    const std::size_t audioTracksAfterSecond = window.sequence()->audioTracks().size();
    drop(sound, overX, audioRowY(0));
    if (window.sequence()->audioTracks().size() != audioTracksAfterSecond + 1) {
        zaro::app::testing::failf("dropping sound over a busy row did not make one for it\n");
    }
    if (window.sequence()->audioTracks().back().clips().size() != 1) {
        zaro::app::testing::failf("the new audio track did not get the sound\n");
    }
    if (window.sequence()->videoTracks().size() != videoTracksAfterSecond) {
        zaro::app::testing::failf("a sound file made a picture row\n");
    }

    // 4. A sound file let go of over a *picture* row goes to the sound block
    // anyway, for the same reason.
    const std::size_t v1ClipsAfterThird = window.sequence()->videoTracks().front().clips().size();
    std::size_t soundClipsBefore = 0;
    for (const zaro::model::Track& track : window.sequence()->audioTracks()) {
        soundClipsBefore += track.clips().size();
    }
    drop(sound, emptyX, videoRowY(0));
    if (window.sequence()->videoTracks().front().clips().size() != v1ClipsAfterThird) {
        zaro::app::testing::failf("a sound file landed on a picture track\n");
    }
    std::size_t soundClipsAfter = 0;
    for (const zaro::model::Track& track : window.sequence()->audioTracks()) {
        soundClipsAfter += track.clips().size();
    }
    if (soundClipsAfter != soundClipsBefore + 1) {
        zaro::app::testing::failf("the sound file did not fall through to the sound rows\n");
    }

    // One undo step per drop, including the two that had to make a track: the
    // track is part of putting the clip down, not a separate thing anybody
    // asked for.
    if (window.commands().position() != stepsBefore + 4) {
        zaro::app::testing::failf("four drops made %zu undo steps, not four\n",
                                  window.commands().position() - stepsBefore);
    }
    for (int i = 0; i < 4; ++i) {
        window.commands().undo(window.project());
    }
    if (window.sequence()->videoTracks().size() != videoTracksBefore ||
        window.sequence()->audioTracks().size() != audioTracksBefore ||
        window.sequence()->videoTracks().front().clips().size() != v1ClipsBefore ||
        window.sequence()->audioTracks().front().clips().size() != a1ClipsBefore) {
        zaro::app::testing::failf("undoing the four drops did not put the cut back\n");
    }

    std::printf("  dropped on the timeline: picture and sound linked, four drops undone\n");

    timeline->setSnapEnabled(snapWas);
    QApplication::processEvents();
}

/// Which picture row a clip is on, for a test that has just moved it about.
zaro::model::TrackId onPictureTrack(zaro::app::PreviewWindow& window, zaro::model::ClipId clip) {
    for (const zaro::model::Track& track : window.sequence()->videoTracks()) {
        if (track.find(clip) != nullptr) {
            return track.id();
        }
    }
    return {};
}

// A clip dragged from one row to another.
//
// Vertically as well as sideways, which is the half of the gesture the panel
// used to throw away. What this covers is the wiring and the two rules on it:
// a clip may only join a row of its own kind, and whatever is linked to it
// follows the same shift in time while staying on its own track.
TEST_CASE("A clip drags to another row of its own kind", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    auto* timeline = window.timeline();

    // Somewhere to drag to. The fixture has one row of each kind.
    for (const auto kind : {zaro::model::TrackKind::Video, zaro::model::TrackKind::Audio}) {
        auto built = zaro::edit::makeAddTrack(window.project(), window.sequence()->id(), kind,
                                              kind == zaro::model::TrackKind::Video ? "V2" : "A2");
        REQUIRE(built.hasValue());
        window.commands().execute(window.project(), std::move(*built));
        window.commands().breakMerge();
    }
    QApplication::processEvents();

    // Snapping off: these assertions are about where the clip lands, and an
    // edge pulled onto a nearby edit point would be measuring the snap.
    const bool snapWas = timeline->snapEnabled();
    timeline->setSnapEnabled(false);

    const auto v1 = window.sequence()->videoTracks().front().id();
    const auto v2 = window.sequence()->videoTracks().back().id();
    const auto a1 = window.sequence()->audioTracks().front().id();
    const auto moving = window.sequence()->videoTracks().front().clips().front().id;
    const auto partner = window.sequence()->audioTracks().front().clips().front().id;
    timeline->selectOnly(v1, moving);

    const auto rowY = [timeline](zaro::model::TrackId track) {
        const auto row = timeline->rowFor(track);
        REQUIRE(row.has_value());
        return row->top + row->height / 2;
    };

    // A drag that moves in both directions at once, which is the only way to
    // reach another row.
    const auto dragTo = [timeline](int fromX, int fromY, int toX, int toY) {
        const auto send = [&](QEvent::Type type, int x, int y, Qt::MouseButton button,
                              Qt::MouseButtons buttons) {
            QMouseEvent event(type, QPointF(x, y), QPointF(x, y), button, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(timeline, &event);
        };
        send(QEvent::MouseButtonPress, fromX, fromY, Qt::LeftButton, Qt::LeftButton);
        const int steps = 8;
        for (int i = 1; i <= steps; ++i) {
            send(QEvent::MouseMove, fromX + (toX - fromX) * i / steps,
                 fromY + (toY - fromY) * i / steps, Qt::NoButton, Qt::LeftButton);
        }
        send(QEvent::MouseButtonRelease, toX, toY, Qt::LeftButton, Qt::NoButton);
        QApplication::processEvents();
    };

    const zaro::time::Rational rate = window.sequence()->frameRate();
    const auto startBefore = window.sequence()->findTrack(v1)->find(moving)->start();
    const auto partnerBefore = window.sequence()->findTrack(a1)->find(partner)->start();
    const int grabX = static_cast<int>(timeline->layout().xForTime(startBefore)) + 40;
    const int landX = grabX + 60;
    const auto shift =
        timeline->layout().timeForX(landX, rate) - timeline->layout().timeForX(grabX, rate);

    // Up onto the empty picture row.
    const std::size_t stepsBefore = window.commands().position();
    dragTo(grabX, rowY(v1), landX, rowY(v2));

    if (window.sequence()->findTrack(v1)->find(moving) != nullptr) {
        zaro::app::testing::failf("the clip is still on the row it was dragged off\n");
    }
    const zaro::model::Clip* landed = window.sequence()->findTrack(v2)->find(moving);
    if (landed == nullptr) {
        zaro::app::testing::failf("the clip did not arrive on the other picture row\n");
    }
    if (landed->start() != startBefore + shift) {
        zaro::app::testing::failf("it landed at %lld, not at %lld\n",
                                  static_cast<long long>(landed->start().frames()),
                                  static_cast<long long>((startBefore + shift).frames()));
    }

    // Its sound followed by the same amount, and stayed where sound lives.
    const zaro::model::Clip* followed = window.sequence()->findTrack(a1)->find(partner);
    if (followed == nullptr) {
        zaro::app::testing::failf("the linked sound left A1\n");
    }
    if (followed->start() != partnerBefore + shift) {
        zaro::app::testing::failf("the linked sound is at %lld, not at %lld\n",
                                  static_cast<long long>(followed->start().frames()),
                                  static_cast<long long>((partnerBefore + shift).frames()));
    }

    // The whole drag is one undo step, however many rows it crossed.
    if (window.commands().position() != stepsBefore + 1) {
        zaro::app::testing::failf("the drag made %zu undo steps, not one\n",
                                  window.commands().position() - stepsBefore);
    }

    // Down onto a sound row: picture has no business there. The clip follows the
    // pointer as far as the bottom of the picture block and stays on the last
    // row it could legally be on, rather than crossing the boundary.
    const auto beforeCrossing = window.sequence()->findTrack(v2)->find(moving)->start();
    const std::size_t soundClipsBefore = window.sequence()->findTrack(a1)->clips().size();
    const int crossX = static_cast<int>(timeline->layout().xForTime(beforeCrossing)) + 40;
    dragTo(crossX, rowY(v2), crossX + 30, rowY(a1));

    const bool onPicture = std::any_of(
        window.sequence()->videoTracks().begin(), window.sequence()->videoTracks().end(),
        [moving](const zaro::model::Track& track) { return track.find(moving) != nullptr; });
    if (!onPicture) {
        zaro::app::testing::failf("a picture clip was dragged off the picture rows\n");
    }
    if (window.sequence()->findTrack(a1)->clips().size() != soundClipsBefore) {
        zaro::app::testing::failf("the sound row gained a clip it should have refused\n");
    }

    // And the same gesture on the sound side, which is the other half of what
    // "a row of its own kind" means. Its linked picture follows in time and
    // stays where picture lives.
    const auto a2 = window.sequence()->audioTracks().back().id();
    const auto soundBefore = window.sequence()->findTrack(a1)->find(partner)->start();
    const auto pictureTrack = onPictureTrack(window, moving);
    const auto pictureBefore = window.sequence()->findTrack(pictureTrack)->find(moving)->start();
    const int soundX = static_cast<int>(timeline->layout().xForTime(soundBefore)) + 40;
    timeline->selectOnly(a1, partner);
    dragTo(soundX, rowY(a1), soundX + 50, rowY(a2));

    const zaro::model::Clip* movedSound = window.sequence()->findTrack(a2)->find(partner);
    if (movedSound == nullptr) {
        zaro::app::testing::failf("the sound clip did not reach the other sound row\n");
    }
    const auto soundShift = movedSound->start() - soundBefore;
    if (soundShift.frames() == 0) {
        zaro::app::testing::failf("the sound clip changed rows without moving in time\n");
    }
    const zaro::model::Clip* picture = window.sequence()->findTrack(pictureTrack)->find(moving);
    if (picture == nullptr) {
        zaro::app::testing::failf("the linked picture left its row\n");
    }
    if (picture->start() != pictureBefore + soundShift) {
        zaro::app::testing::failf("the linked picture is at %lld, not at %lld\n",
                                  static_cast<long long>(picture->start().frames()),
                                  static_cast<long long>((pictureBefore + soundShift).frames()));
    }

    std::printf("  clip moved between rows: V1 to V2 and A1 to A2, each partner following\n");

    timeline->setSnapEnabled(snapWas);
    QApplication::processEvents();
}
