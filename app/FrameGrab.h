// Driving the window from code: grab what it drew, and move the mouse over it.
//
// Shared by main.cpp's --selftest and by the GUI tests in app/tests, which is
// why these live here rather than in either. They are the two things any test
// that wants to check a picture needs, and getting either subtly wrong -- a
// grab that happens before the repaint, a drag delivered as a single jump --
// produces a test that passes or fails for reasons that have nothing to do
// with the code under it.
#pragma once

#include <QApplication>
#include <QEventLoop>
#include <QImage>
#include <QMouseEvent>
#include <QPointF>
#include <cstdint>

#include "ProgramMonitor.h"
#include "TimelineWidget.h"

namespace zaro::app {

/// Grab what the monitor is showing, once it has actually drawn it.
///
/// One `processEvents` is not a guarantee of a repaint: the widget schedules
/// one, and whether it happens before the grab depends on what else the event
/// loop has to do. A scan that grabs too early reads the frame before the
/// playhead moved -- or a blank one, if nothing has been drawn yet -- and the
/// result is a check that usually passes and occasionally reports that a lit
/// fixture is entirely black.
///
/// Bounded: if the monitor never draws, the scan should report what it saw
/// rather than hang.
inline QImage settledGrab(app::ProgramMonitor* monitor) {
    const std::int64_t before = monitor->framesRendered();
    monitor->update();
    for (int spin = 0; spin < 100 && monitor->framesRendered() == before; ++spin) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return monitor->grabFramebuffer();
}

/// Drive a real drag through the widget, as the mouse would.
inline void dragOnTimeline(app::TimelineWidget* timeline, int fromX, int toX, int y,
                           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const auto press = [&](QEvent::Type type, int x, Qt::MouseButton button,
                           Qt::MouseButtons buttons) {
        QMouseEvent event(type, QPointF(x, y), QPointF(x, y), button, buttons, modifiers);
        QCoreApplication::sendEvent(timeline, &event);
    };
    press(QEvent::MouseButtonPress, fromX, Qt::LeftButton, Qt::LeftButton);
    // In steps, because a trim is applied incrementally and a single jump would
    // not exercise the accumulation the real interaction relies on.
    const int steps = 8;
    for (int i = 1; i <= steps; ++i) {
        press(QEvent::MouseMove, fromX + (toX - fromX) * i / steps, Qt::NoButton, Qt::LeftButton);
    }
    press(QEvent::MouseButtonRelease, toX, Qt::LeftButton, Qt::NoButton);
}

}  // namespace zaro::app
