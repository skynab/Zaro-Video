// Playback: the transport, and the clock that drives the playhead.
//
// ADR-006 makes the audio device the clock, which is why these are the only
// tests here that care whether the machine has a sound card. Without one the
// transport still runs and still reports that it is playing -- there is simply
// no elapsed-sample count for the position to be a function of, so nothing
// advances. That is the design working, not a failure, and the checks that
// depend on a moving playhead say so rather than failing on a build machine.

#include <catch2/catch_test_macros.hpp>

#include "../FrameGrab.h"
#include "GuiFixture.h"

// The suite was written inside main(), which had this at file scope; the
// bodies still say `model::` and `Status` unqualified.
using namespace zaro;

using zaro::app::dragOnTimeline;
using zaro::app::settledGrab;

namespace {

/// Run the event loop for a while, the way a person watching would.
void pumpFor(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    }
}

}  // namespace

TEST_CASE("Play and pause through the transport", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;

    REQUIRE_FALSE(window.playback().isPlaying());
    const auto started = window.position();

    REQUIRE(window.trigger("play-pause"));
    QApplication::processEvents();
    if (!window.playback().isPlaying()) {
        zaro::app::testing::failf("the transport did not start\n");
    }

    pumpFor(400);
    const auto reached = window.position();

    REQUIRE(window.trigger("play-pause"));
    QApplication::processEvents();
    if (window.playback().isPlaying()) {
        zaro::app::testing::failf("the transport did not stop\n");
    }

    // Where it stopped is where it stays: a pause that keeps drifting for a
    // frame or two is the clock still being read after the device went quiet.
    pumpFor(120);
    if (window.position() != reached) {
        zaro::app::testing::failf("the playhead moved after pausing: %lld then %lld\n",
                                  static_cast<long long>(reached.frames()),
                                  static_cast<long long>(window.position().frames()));
    }

    if (!window.playback().hasAudioClock()) {
        std::printf("  playback: started and stopped; no audio device, so no clock to advance\n");
        return;
    }
    std::printf("  playback: ran from %lld to %lld in 400ms\n",
                static_cast<long long>(started.frames()), static_cast<long long>(reached.frames()));
    if (reached.frames() <= started.frames()) {
        zaro::app::testing::failf(
            "the clock is running but the playhead did not advance: still at %lld\n",
            static_cast<long long>(reached.frames()));
    }
}

TEST_CASE("Shuttling picks up a new speed without the playhead jumping", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;

    if (!window.playback().hasAudioClock()) {
        SKIP("no audio device: there is no clock for a shuttle to change the speed of");
    }

    REQUIRE(window.trigger("play-pause"));
    pumpFor(200);
    const auto beforeShuttle = window.position();

    // L a second time is double speed. What is checked is that changing speed
    // re-anchors rather than restarting: the playhead must carry on from where
    // it was, not snap back to where playback began.
    REQUIRE(window.trigger("shuttle-forward"));
    QApplication::processEvents();
    const auto justAfter = window.position();
    if (justAfter.frames() < beforeShuttle.frames()) {
        zaro::app::testing::failf("changing speed sent the playhead backwards: %lld then %lld\n",
                                  static_cast<long long>(beforeShuttle.frames()),
                                  static_cast<long long>(justAfter.frames()));
    }

    pumpFor(200);
    const auto reached = window.position();
    std::printf("  shuttle: %lld at 1x, %lld after changing speed, %lld 200ms later\n",
                static_cast<long long>(beforeShuttle.frames()),
                static_cast<long long>(justAfter.frames()),
                static_cast<long long>(reached.frames()));

    REQUIRE(window.trigger("shuttle-stop"));
    QApplication::processEvents();
    if (window.playback().isPlaying()) {
        zaro::app::testing::failf("stopping the shuttle did not stop the transport\n");
    }
}
