#include <catch2/catch_test_macros.hpp>

#include "zaro/core/playback/Transport.h"

using namespace zaro;
using playback::Transport;
using playback::TransportState;

TEST_CASE("L shuttles forward up the ladder", "[playback][transport]") {
    Transport transport;
    CHECK(transport.state() == TransportState::Stopped);
    CHECK(transport.speed().isZero());

    transport.pressL();
    CHECK(transport.isPlaying());
    CHECK(transport.speed() == time::Rational::fromInt(1));

    transport.pressL();
    CHECK(transport.speed() == time::Rational::fromInt(2));
    transport.pressL();
    CHECK(transport.speed() == time::Rational::fromInt(4));
    transport.pressL();
    CHECK(transport.speed() == time::Rational::fromInt(8));

    SECTION("and stops at the top rather than wrapping to 1x") {
        transport.pressL();
        transport.pressL();
        CHECK(transport.speed() == time::Rational::fromInt(8));
        CHECK(transport.rung() == 3);
    }
}

TEST_CASE("J mirrors L in reverse", "[playback][transport]") {
    Transport transport;
    transport.pressJ();
    CHECK(transport.speed() == time::Rational::fromInt(-1));
    transport.pressJ();
    CHECK(transport.speed() == time::Rational::fromInt(-2));
    CHECK(transport.isPlaying());
}

TEST_CASE("Reversing direction restarts at 1x", "[playback][transport]") {
    // Pressing J while running forward at 8x should go to -1x, not -8x: it is
    // a change of mind, not a continuation of the shuttle.
    Transport transport;
    transport.pressL();
    transport.pressL();
    transport.pressL();
    REQUIRE(transport.speed() == time::Rational::fromInt(4));

    transport.pressJ();
    CHECK(transport.speed() == time::Rational::fromInt(-1));
    CHECK(transport.rung() == 0);
}

TEST_CASE("K pauses and resets the shuttle", "[playback][transport]") {
    Transport transport;
    transport.pressL();
    transport.pressL();
    transport.pressL();

    transport.pressK();
    CHECK(transport.state() == TransportState::Paused);
    CHECK(transport.speed().isZero());

    // After a pause, L means play, not resume 4x.
    transport.pressL();
    CHECK(transport.speed() == time::Rational::fromInt(1));
}

TEST_CASE("Slow shuttle is an exact fraction", "[playback][transport]") {
    Transport transport;
    transport.pressSlowForward();
    CHECK(transport.speed() == time::Rational{1, 4});
    transport.pressSlowReverse();
    CHECK(transport.speed() == time::Rational{-1, 4});
    // Rational, not 0.25 as a double: speed multiplies into the position
    // mapping, and a fraction that cannot be represented exactly puts the
    // playhead visibly out after a minute.
    CHECK(transport.speed().den() == 4);
}

TEST_CASE("Play, pause and stop", "[playback][transport]") {
    Transport transport;
    transport.play();
    CHECK(transport.isPlaying());
    CHECK(transport.speed() == time::Rational::fromInt(1));

    transport.togglePlayPause();
    CHECK(transport.state() == TransportState::Paused);
    transport.togglePlayPause();
    CHECK(transport.isPlaying());

    transport.stop();
    CHECK(transport.state() == TransportState::Stopped);
    CHECK(transport.speed().isZero());
}

TEST_CASE("The ladder is configurable", "[playback][transport]") {
    Transport::Config config;
    config.ladder = {time::Rational::fromInt(1), time::Rational::fromInt(3)};
    Transport transport{config};

    transport.pressL();
    CHECK(transport.speed() == time::Rational::fromInt(1));
    transport.pressL();
    CHECK(transport.speed() == time::Rational::fromInt(3));
    transport.pressL();
    CHECK(transport.speed() == time::Rational::fromInt(3));
}
