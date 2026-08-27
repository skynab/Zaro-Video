// The window the GUI tests drive, and the few things every one of them needs.
//
// These tests are not unit tests and are not trying to be. What they cover is
// the wiring: that a gesture on a widget reaches the right edit operation, and
// that the result comes back out through the real compositor as a picture that
// changed in the way it should have. The operations underneath are covered
// headlessly in core/tests, which is where a question about the arithmetic
// belongs -- what those cannot see is whether anything is connected to them.
//
// One window, shared. Building it costs a GPU context, a decode of the fixture
// media and a waveform pass, and doing that sixty times would turn a suite that
// runs in a minute into one nobody waits for. What makes sharing safe is that
// every test leaves the command stack where it found it: see Rewind.
#pragma once

#include <QImage>
#include <optional>
#include <string>

#include "zaro/ui/TimelineLayout.h"

#include "../PreviewWindow.h"

namespace zaro::app::testing {

/// The one window. Built on the first call, torn down when the process ends.
///
/// Deliberately returns nothing but the window. An earlier version of this
/// handed out references to the sequence and to V1 as well, bound once when the
/// fixture was built -- and a test that adds a track or a sequence reallocates
/// the vector underneath them, so the test that ran next read freed memory. It
/// held together only because the sections ran in a fixed order with the two
/// that reallocate placed last, which is not a property a suite of independent
/// tests has. Everything else is looked up fresh, per test, from here.
PreviewWindow& gui();

/// The absolute path of a generated media fixture.
///
/// Absolute because ctest runs a test from the build directory, not from the
/// root of the checkout, and the relative paths these tests were written with
/// worked only when the binary happened to be started by hand from there.
std::string mediaFixture(const char* name);

/// Mean brightness of a grabbed frame, 0..255.
///
/// Most of what these tests assert is "this got darker" or "this got brighter",
/// because that is what survives the difference between one GPU and another. An
/// exact pixel comparison would be a test of the driver.
double meanGray(const QImage& image);

/// Fail the test with a printf-formatted message.
///
/// The messages were written as fprintf calls when this suite was a block
/// inside main(), and they are good messages -- they say what was measured as
/// well as what was expected. This keeps them and hands them to Catch2 instead
/// of to stderr followed by a return.
[[noreturn]] void failf(const char* format, ...);

/// Undo everything the test did, however it leaves.
///
/// Most tests already end by rewinding their own command stack. This runs on
/// the way out of the scope, so the rewind also happens when an assertion has
/// thrown -- which is the case the hand-written version could never cover,
/// since a failure returned out of main and there was no next test to protect.
struct Rewind {
    ~Rewind();
};

/// Put the window back the way the fixture built it.
///
/// Undo is not the whole story: the active sequence is not a command, and the
/// tests for New and Open replace the project outright. This is called before
/// every test rather than after, so that a test that fails partway through
/// cannot leave the next one reading a project it knows nothing about.
void restoreFixtureProject();

}  // namespace zaro::app::testing
