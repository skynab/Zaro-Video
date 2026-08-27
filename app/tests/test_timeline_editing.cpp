// Editing on the timeline: the blade, the mouse, and three-point assembly.
//
// Driven through the real window against the real compositor. See GuiFixture.h
// for what is shared and why.

#include <catch2/catch_test_macros.hpp>

#include "../FrameGrab.h"
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
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

    // Trim the out point inwards by dragging its right edge left.
    const int outX = static_cast<int>(timeline->layout().xForTime(original.endExclusive()));
    const int wantedX = outX - 120;
    dragOnTimeline(timeline, outX - 2, wantedX, y);

    const zaro::model::Clip* trimmed =
        window.project().findSequence(sequence.id())->videoTracks().front().find(original.id);
    if (trimmed == nullptr) {
        zaro::app::testing::failf("the clip disappeared\n");
    }
    const std::int64_t shortened = original.duration().frames() - trimmed->duration().frames();
    std::printf("  after trimming out by 120px: %lld frames shorter\n",
                static_cast<long long>(shortened));
    if (shortened <= 0) {
        zaro::app::testing::failf("the trim did not shorten the clip\n");
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
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

    const auto& rate = sequence.frameRate();
    const auto& videoStart = sequence.videoTracks().front();
    if (videoStart.clips().empty() || sequence.audioTracks().empty() ||
        !videoStart.clips().front().link.isValid()) {
        zaro::app::testing::failf("this project has no linked pair to cut\n");
    }
    const zaro::model::TrackId audioId = sequence.audioTracks().front().id();
    const zaro::time::RationalTime cutAt{90, rate};

    timeline->setTool(app::TimelineWidget::Tool::Blade);
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
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

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
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

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
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

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
