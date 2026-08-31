// Effects and keyframes: what the parameter panel writes, and what comes out.
//
// Driven through the real window against the real compositor. See GuiFixture.h
// for what is shared and why.

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFocusEvent>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QToolButton>
#include <cmath>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "../CurveEditor.h"
#include "../FrameGrab.h"
#include "GuiFixture.h"

// The suite was written inside main(), which had this at file scope; the
// bodies still say `model::` and `Status` unqualified.
using namespace zaro;

using zaro::app::dragOnTimeline;
using zaro::app::settledGrab;

// meanGray is named here rather than aliased inside each test, which is how it
// arrived: the suite was one main() sharing local lambdas, and the conversion
// left every test opening with a reference bound to this function. MSVC's
// constexpr evaluator crashes on a call made through such a reference when the
// result initialises a const double -- an internal compiler error, not a
// diagnostic -- so `const double bright = meanGray(image);` took the whole
// build down. Calling the function by its own name is what every other
// compiler was doing anyway.
using zaro::app::testing::meanGray;

// A parameter change has to reach the picture, not just the model.
// Rendering the same frame at full and at low opacity should differ;
// if they do not, the compositor is not seeing what the panel wrote.
TEST_CASE("Lowering opacity reaches the compositor", "[gui]") {
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

    window.setPosition(
        zaro::time::RationalTime{sequence.duration().frames() / 2, sequence.frameRate()});
    QApplication::processEvents();
    const QImage before = settledGrab(window.monitor());

    zaro::model::Transform faded;
    faded.opacity = 0.15;
    auto dim = zaro::edit::makeSetTransform(window.project(), {sequence.id(), videoTrack.id()},
                                            original.id, faded);
    if (!dim) {
        zaro::app::testing::failf("%s\n", dim.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*dim));
    window.monitor()->update();
    QApplication::processEvents();
    const QImage after = settledGrab(window.monitor());

    const double brightBefore = meanGray(before);
    const double brightAfter = meanGray(after);
    std::printf("  opacity 1.0 -> 0.15 changed mean brightness %.1f -> %.1f\n", brightBefore,
                brightAfter);
    if (!(brightAfter < brightBefore * 0.6)) {
        zaro::app::testing::failf(
            "lowering opacity did not darken the picture; the panel and "
            "the compositor are not connected\n");
    }
}

// Keyframes have to reach the GPU compositor, not just the CPU one.
// The two traversals are separate code, so a curve honoured on export
// and ignored in preview is a bug nothing else here would catch: the
// headless render tests only exercise render::RenderGraph.
TEST_CASE("A keyframed fade reaches the GPU compositor", "[gui]") {
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

    // Undo the dim above before taking any pointer into the model: undo
    // restores a snapshot, which replaces the clips wholesale.
    window.commands().undo(window.project());
    QApplication::processEvents();

    const auto brightnessAt = [&](std::int64_t frame) {
        window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
        return meanGray(settledGrab(window.monitor()));
    };

    // This fixture is black except on its flash frames, so a fade has to
    // be measured on a frame that is lit to begin with. Measuring an
    // arbitrary frame would report a working fade on footage that was
    // already black.
    // Not from frame zero: the fade is anchored at the clip's first
    // frame, so a lit frame there would give the ramp no length at all
    // and both keyframes would land on the same instant.
    std::int64_t litFrame = -1;
    double baseline = 0.0;
    for (std::int64_t frame = 8; frame < 60; ++frame) {
        const double gray = brightnessAt(frame);
        if (gray > baseline) {
            baseline = gray;
            litFrame = frame;
        }
    }
    if (litFrame < 0 || baseline < 40.0) {
        zaro::app::testing::failf("no lit frame to fade\n");
    }

    zaro::model::Clip* clip = window.project()
                                  .findSequence(sequence.id())
                                  ->tracksMutable(zaro::model::TrackKind::Video)
                                  .front()
                                  .find(original.id);
    if (clip == nullptr) {
        zaro::app::testing::failf("the clip vanished\n");
    }

    // Keyframes are in source time. Anchoring the fade so that the lit
    // frame lands at a known point on the curve is what makes this a
    // measurement of the interpolation rather than of the footage.
    const zaro::time::RationalTime litSource =
        clip->sourceTimeAt(zaro::time::RationalTime{litFrame, sequence.frameRate()});
    const auto setFade = [&](std::int64_t spanFrames) {
        zaro::model::Keyframe lit;
        lit.time = clip->sourceRange.start();
        lit.value = 1.0;
        zaro::model::Keyframe dark;
        dark.time =
            clip->sourceRange.start() + zaro::time::RationalTime{spanFrames, litSource.rate()};
        dark.value = 0.0;
        clip->animation.erase(zaro::model::Param::Opacity);
        clip->animation.curve(zaro::model::Param::Opacity).set(lit);
        clip->animation.curve(zaro::model::Param::Opacity).set(dark);
    };

    const std::int64_t intoClip = litSource.frames() - clip->sourceRange.start().frames();
    setFade(intoClip * 2);  // the lit frame sits halfway down the ramp
    const double halfway = brightnessAt(litFrame);
    setFade(intoClip);  // and now exactly at its end
    const double gone = brightnessAt(litFrame);
    clip->animation.erase(zaro::model::Param::Opacity);

    std::printf("  keyframed fade on the GPU: %.1f lit, %.1f halfway, %.1f faded\n", baseline,
                halfway, gone);
    if (!(halfway > baseline * 0.3) || !(halfway < baseline * 0.75)) {
        zaro::app::testing::failf("the GPU compositor is not interpolating keyframes\n");
    }
    if (!(gone < baseline * 0.1)) {
        zaro::app::testing::failf(
            "the GPU compositor is not reading keyframes; preview and "
            "export would disagree\n");
    }
}

// A generated shape, created and then edited through the panel, and
// measured through the real GPU compositor. A shape is drawn on the CPU
// and uploaded, so this also checks that path is reachable at all.
TEST_CASE("A shape layer, created and edited through the panel", "[gui]") {
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

    const auto videoTrackId = videoTrack.id();

    // The darkest frame is chosen *before* the shape is added: with a
    // white rectangle covering the frame every position reads the same,
    // and the search would settle on whichever came first -- which on
    // this fixture is a flash frame that is white anyway.
    std::int64_t darkFrame = 0;
    double darkest = 1e9;
    for (std::int64_t frame = 0; frame < 35; ++frame) {
        window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
        const double gray = meanGray(settledGrab(window.monitor()));
        if (gray < darkest) {
            darkest = gray;
            darkFrame = frame;
        }
    }
    window.setPosition(zaro::time::RationalTime{darkFrame, sequence.frameRate()});
    QApplication::processEvents();
    const double without = meanGray(settledGrab(window.monitor()));

    zaro::model::Graphic shape;
    shape.kind = zaro::model::GraphicKind::Rectangle;
    shape.width = 4000.0;  // larger than the frame, so it fills it
    shape.height = 4000.0;
    shape.red = 1.0;
    shape.green = 1.0;
    shape.blue = 1.0;

    const auto& videoTracks = window.project().findSequence(sequence.id())->videoTracks();
    const auto topTrack = videoTracks.size() > 1 ? videoTracks[1].id() : videoTrackId;
    auto built = zaro::edit::makeAddGraphic(
        window.project(), {sequence.id(), topTrack}, shape,
        zaro::time::TimeRange{zaro::time::RationalTime{0, sequence.frameRate()},
                              zaro::time::RationalTime{40, sequence.frameRate()}});
    if (!built) {
        zaro::app::testing::failf("%s\n", built.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*built));
    window.monitor()->update();
    QApplication::processEvents();
    const double withShape = meanGray(settledGrab(window.monitor()));

    std::printf("  shape layer: %.1f with a white rectangle, %.1f without\n", withShape, without);
    if (!(withShape > without + 50.0)) {
        zaro::app::testing::failf("the shape layer did not reach the preview\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.monitor()->update();
    QApplication::processEvents();
}

// The effect stack, added and ordered through the panel's own buttons.
//
// Measured by counting bright pixels rather than by mean brightness: a
// blur redistributes light rather than removing it, so the mean is very
// nearly unchanged and a test of it would pass on a blur that did
// nothing. What a blur actually does is turn hard edges into gradients,
// and that shows up as fewer pixels at full brightness.
TEST_CASE("The effect stack, added and ordered through the panel", "[gui]") {
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

    const auto brightPixels = [](const QImage& image, int threshold) {
        int count = 0;
        for (int scanY = 0; scanY < image.height(); ++scanY) {
            for (int x = 0; x < image.width(); ++x) {
                if (qGray(image.pixel(x, scanY)) >= threshold) {
                    ++count;
                }
            }
        }
        return count;
    };

    const auto fxSequenceId = window.project().activeSequence();
    const auto& fxTracks = window.project().findSequence(fxSequenceId)->videoTracks();
    const auto fxTop = fxTracks.size() > 1 ? fxTracks[1].id() : fxTracks.front().id();

    window.setPosition(zaro::time::RationalTime{5, sequence.frameRate()});
    QApplication::processEvents();

    // A white rectangle well inside the frame, so it has edges of its
    // own for a blur to soften.
    zaro::model::Graphic block;
    block.kind = zaro::model::GraphicKind::Rectangle;
    // The fixture's sequence is small, so the rectangle is too: it has
    // to sit well inside the frame to have edges of its own.
    block.width = 120.0;
    block.height = 80.0;
    block.red = 1.0;
    block.green = 1.0;
    block.blue = 1.0;
    auto placed = zaro::edit::makeAddGraphic(
        window.project(), {fxSequenceId, fxTop}, block,
        zaro::time::TimeRange{zaro::time::RationalTime{0, sequence.frameRate()},
                              zaro::time::RationalTime{40, sequence.frameRate()}});
    if (!placed) {
        zaro::app::testing::failf("%s\n", placed.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*placed));
    const auto fxClipId =
        window.project().findSequence(fxSequenceId)->findTrack(fxTop)->clips().front().id;
    window.monitor()->update();
    QApplication::processEvents();
    const int hardEdges = brightPixels(settledGrab(window.monitor()), 200);
    if (hardEdges < 100) {
        zaro::app::testing::failf("the rectangle did not reach the preview\n");
    }

    timeline->selectOnly(fxTop, fxClipId);
    window.effects()->setSelection(fxTop, fxClipId);
    QApplication::processEvents();

    auto* kindBox = window.effects()->findChild<QComboBox*>("effect-kind");
    auto* addButton = window.effects()->findChild<QPushButton*>("effect-add");
    auto* firstParam = window.effects()->findChild<QDoubleSpinBox*>("effect-param-0");
    auto* enabledBox = window.effects()->findChild<QCheckBox*>("effect-enabled");
    if (kindBox == nullptr || addButton == nullptr || firstParam == nullptr ||
        enabledBox == nullptr) {
        zaro::app::testing::failf("the effect controls are not in the panel\n");
    }

    kindBox->setCurrentIndex(kindBox->findData(static_cast<int>(zaro::model::EffectKind::Blur)));
    addButton->click();
    QApplication::processEvents();
    if (window.project()
            .findSequence(fxSequenceId)
            ->findTrack(fxTop)
            ->find(fxClipId)
            ->effects.size() != 1) {
        zaro::app::testing::failf("adding an effect did not reach the clip\n");
    }
    // Adding one must not change the picture: what it does and what it
    // is set to have to be tellable apart.
    window.monitor()->update();
    QApplication::processEvents();
    const int justAdded = brightPixels(settledGrab(window.monitor()), 200);
    if (std::abs(justAdded - hardEdges) > hardEdges / 20) {
        zaro::app::testing::failf("adding an effect changed the picture by itself\n");
    }

    firstParam->setValue(6.0);  // radius, in output pixels
    QApplication::processEvents();
    window.monitor()->update();
    QApplication::processEvents();
    const int blurred = brightPixels(settledGrab(window.monitor()), 200);

    std::printf("  effect stack: %d bright pixels sharp, %d after a blur\n", hardEdges, blurred);
    if (!(blurred < hardEdges * 9 / 10)) {
        zaro::app::testing::failf("the blur did not reach the picture\n");
    }

    // And switching it off brings the edges back, so what was measured
    // was the effect rather than the clip changing for another reason.
    enabledBox->setChecked(false);
    QApplication::processEvents();
    window.monitor()->update();
    QApplication::processEvents();
    if (brightPixels(settledGrab(window.monitor()), 200) < hardEdges * 9 / 10) {
        zaro::app::testing::failf("disabling the effect did not restore the edges\n");
    }
    enabledBox->setChecked(true);
    QApplication::processEvents();

    // Now animate it, through the same stopwatch every other parameter
    // has. A ramp from nothing to a wide blur, and the picture has to
    // differ along it.
    auto* stopwatch = window.effects()->findChild<QToolButton*>("effect-stopwatch-0");
    if (stopwatch == nullptr) {
        zaro::app::testing::failf("the effect parameter has no stopwatch\n");
    }
    window.setPosition(zaro::time::RationalTime{2, sequence.frameRate()});
    QApplication::processEvents();
    firstParam->setValue(0.0);
    QApplication::processEvents();
    stopwatch->click();
    QApplication::processEvents();

    const auto* animatedClip =
        window.project().findSequence(fxSequenceId)->findTrack(fxTop)->find(fxClipId);
    if (animatedClip->effects.empty() ||
        !animatedClip->effects.front().isAnimated(zaro::model::EffectParam::Radius)) {
        zaro::app::testing::failf("the stopwatch did not animate the parameter\n");
    }

    // A second keyframe further along, by moving the playhead and
    // typing a value -- which on an animated parameter has to write a
    // keyframe rather than a static value nothing would read.
    window.setPosition(zaro::time::RationalTime{30, sequence.frameRate()});
    QApplication::processEvents();
    firstParam->setValue(8.0);
    QApplication::processEvents();

    window.setPosition(zaro::time::RationalTime{2, sequence.frameRate()});
    QApplication::processEvents();
    window.monitor()->update();
    QApplication::processEvents();
    const int rampStart = brightPixels(settledGrab(window.monitor()), 200);

    window.setPosition(zaro::time::RationalTime{30, sequence.frameRate()});
    QApplication::processEvents();
    window.monitor()->update();
    QApplication::processEvents();
    const int rampEnd = brightPixels(settledGrab(window.monitor()), 200);

    std::printf(
        "  keyframed blur: %d bright pixels at the start of the ramp, %d at the "
        "end\n",
        rampStart, rampEnd);
    if (!(rampEnd < rampStart * 9 / 10)) {
        zaro::app::testing::failf("the keyframed blur is not ramping\n");
    }

    // Lens distortion, added through the same generic controls: if
    // adding an effect is really data, this needed no new widgets.
    enabledBox->setChecked(false);  // put the blur out of the way
    QApplication::processEvents();
    kindBox->setCurrentIndex(kindBox->findData(static_cast<int>(zaro::model::EffectKind::Distort)));
    addButton->click();
    QApplication::processEvents();
    auto* effectList = window.effects()->findChild<QListWidget*>("effect-list");
    if (effectList == nullptr || effectList->count() != 2) {
        zaro::app::testing::failf("adding a distortion did not reach the stack\n");
    }
    effectList->setCurrentRow(1);
    QApplication::processEvents();
    const auto* distorted =
        window.project().findSequence(fxSequenceId)->findTrack(fxTop)->find(fxClipId);
    if (distorted->effects.back().kind != zaro::model::EffectKind::Distort) {
        zaro::app::testing::failf("the second effect is not the distortion\n");
    }

    window.setPosition(zaro::time::RationalTime{2, sequence.frameRate()});
    window.renderCache().clear();
    QApplication::processEvents();
    const int straight = brightPixels(settledGrab(window.monitor()), 200);

    // Pulling the picture in towards the centre shrinks the rectangle,
    // so fewer pixels are lit. The parameter rows are the generic ones:
    // curvature is the first parameter of a distortion, where radius is
    // the first parameter of a blur.
    firstParam->setValue(0.8);
    window.renderCache().clear();
    QApplication::processEvents();
    const int bent = brightPixels(settledGrab(window.monitor()), 200);

    std::printf("  lens distortion: %d bright pixels straight, %d bent in\n", straight, bent);
    if (!(bent < straight * 9 / 10)) {
        zaro::app::testing::failf("the distortion did not reach the picture\n");
    }
    // And the zoom, the second parameter, puts the size back.
    auto* secondParam = window.effects()->findChild<QDoubleSpinBox*>("effect-param-1");
    if (secondParam == nullptr) {
        zaro::app::testing::failf("the distortion has no second parameter\n");
    }
    secondParam->setValue(1.6);
    window.renderCache().clear();
    QApplication::processEvents();
    const int refilled = brightPixels(settledGrab(window.monitor()), 200);
    if (!(refilled > bent)) {
        zaro::app::testing::failf(
            "zooming a distorted clip did not fill the frame back "
            "(%d vs %d)\n",
            refilled, bent);
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.monitor()->update();
    QApplication::processEvents();
}

// The curve editor, driven with the mouse. The curve engine is tested
// headlessly and the two render paths are compared against each other;
// what neither of those can see is whether dragging in this widget
// reaches any of it.
TEST_CASE("The curve editor, driven with the mouse", "[gui]") {
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

    auto* editor = window.effects()->findChild<app::CurveEditor*>();
    if (editor == nullptr) {
        zaro::app::testing::failf("there is no curve editor\n");
    }
    window.effects()->setSelection(videoTrack.id(), original.id);
    QApplication::processEvents();

    const auto clipNow = [&]() {
        return window.project()
            .findSequence(sequence.id())
            ->videoTracks()
            .front()
            .find(original.id);
    };
    if (!clipNow()->curves.isIdentity()) {
        zaro::app::testing::failf("the clip starts with a curve on it\n");
    }

    // Grab the black point at the bottom-left and lift it. That is the
    // change this fixture can actually show: it is flashes on black, so
    // a midtone adjustment moves almost nothing, while lifting black
    // moves nearly every pixel.
    const QRect plot = editor->plotArea();
    const QPointF middle(plot.left(), plot.bottom());
    const QPointF lifted(middle.x(), middle.y() - (plot.height() * 0.4));
    {
        QMouseEvent press(QEvent::MouseButtonPress, middle, middle, Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(editor, &press);
        QMouseEvent move(QEvent::MouseMove, lifted, lifted, Qt::NoButton, Qt::LeftButton,
                         Qt::NoModifier);
        QCoreApplication::sendEvent(editor, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, lifted, lifted, Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(editor, &release);
    }
    QApplication::processEvents();

    const zaro::model::ToneCurve& master = clipNow()->curves.master;
    std::printf("  curve editor: %zu points, black lifted to %.3f\n", master.size(),
                master.valueAt(0.0));
    if (master.size() < 2) {
        zaro::app::testing::failf("the curve editor did not give the curve its endpoints\n");
    }
    if (!(master.valueAt(0.0) > 0.2)) {
        zaro::app::testing::failf(
            "dragging upward did not lift the curve; the widget's y "
            "axis is inverted or it is not reaching the model\n");
    }
    if (master.points().front().x != 0.0) {
        zaro::app::testing::failf(
            "the black point moved sideways; the endpoints are supposed "
            "to be pinned in x\n");
    }

    // And it has to reach the picture, not only the model. Measured on
    // a *dark* frame: this fixture is flashes on black, its lit frames
    // are saturated white, and lifting the black point cannot change
    // white at all. On a black frame the same lift moves every pixel.
    // Both extremes of the fixture. The bright one is the reference the
    // lift is judged against: an absolute threshold would be measuring
    // how much of the monitor the letterbox covers, which moves
    // whenever a panel is added -- it has already been wrong twice.
    std::int64_t darkFrame = 0;
    double darkness = 1e9;
    double brightest = 0.0;
    for (std::int64_t frame = 0; frame < 40; ++frame) {
        window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
        const double gray = meanGray(settledGrab(window.monitor()));
        if (gray < darkness) {
            darkness = gray;
            darkFrame = frame;
        }
        brightest = std::max(brightest, gray);
    }
    window.setPosition(zaro::time::RationalTime{darkFrame, sequence.frameRate()});
    for (int i = 0; i < 3; ++i) {
        window.monitor()->update();
        QApplication::processEvents();
    }
    const double withCurve = meanGray(settledGrab(window.monitor()));

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    for (int i = 0; i < 3; ++i) {
        window.monitor()->update();
        QApplication::processEvents();
    }
    const double withoutCurve = meanGray(settledGrab(window.monitor()));
    std::printf("  curve on the GPU: %.1f with, %.1f without\n", withCurve, withoutCurve);
    if (!(withCurve > withoutCurve + 1.0)) {
        zaro::app::testing::failf(
            "the curve does not reach the preview; it would show on "
            "export and not on screen\n");
    }
    window.effects()->refresh();
}

// Keyframing, driven through the panel and the timeline rather than by
// calling the operations: the stopwatch, a value typed at a second
// playhead position, and then dragging the diamond that appears.
TEST_CASE("Keyframing through the panel and the timeline", "[gui]") {
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

    auto* stopwatch = window.effects()->findChild<QToolButton*>("stopwatch:opacity");
    auto* keyButton = window.effects()->findChild<QToolButton*>("keyframe:opacity");
    if (stopwatch == nullptr || keyButton == nullptr) {
        zaro::app::testing::failf("the opacity stopwatch is missing\n");
    }

    window.effects()->setSelection(videoTrack.id(), original.id);
    window.setPosition(zaro::time::RationalTime{10, sequence.frameRate()});
    QApplication::processEvents();

    // The diamonds are the only sign in the timeline that a clip is
    // animated, and a painting bug there is invisible to every other
    // check. Counted inside the keyframe lane only: the clip names and
    // the ruler are drawn in almost the same near-white, and counting
    // the whole widget measures the text rather than the keyframes.
    const auto lanePixels = [&] {
        const QImage shot = timeline->grab().toImage();
        const auto laneRow = timeline->rowFor(videoTrack.id());
        if (!laneRow) {
            return std::int64_t{-1};
        }
        const auto dpr = static_cast<int>(shot.devicePixelRatio());
        const int lane = timeline->layout().keyframeLaneHeight();
        const int top = (laneRow->top + laneRow->height - lane) * dpr;
        const int bottom = std::min((laneRow->top + laneRow->height) * dpr, shot.height());
        std::int64_t found = 0;
        for (int scanY = std::max(0, top); scanY < bottom; ++scanY) {
            // Asked of the theme rather than written out: the diamond
            // is painted in a token, and a literal here would have to
            // be chased every time the palette moves.
            const QColor diamond = zaro::app::theme::neutral(200);
            for (int x = 0; x < shot.width(); ++x) {
                const QColor pixel = shot.pixelColor(x, scanY);
                if (std::abs(pixel.red() - diamond.red()) <= 6 &&
                    std::abs(pixel.green() - diamond.green()) <= 6 &&
                    std::abs(pixel.blue() - diamond.blue()) <= 6) {
                    ++found;
                }
            }
        }
        return found;
    };
    const std::int64_t bareLane = lanePixels();

    if (!stopwatch->isEnabled() || stopwatch->isChecked()) {
        zaro::app::testing::failf("the stopwatch is not offering to animate\n");
    }
    stopwatch->click();
    QApplication::processEvents();

    const auto clipNow = [&]() {
        return window.project()
            .findSequence(sequence.id())
            ->videoTracks()
            .front()
            .find(original.id);
    };
    const zaro::model::Curve* curve = clipNow()->animation.find(zaro::model::Param::Opacity);
    if (curve == nullptr || curve->size() != 1) {
        zaro::app::testing::failf("the stopwatch did not drop a keyframe\n");
    }
    if (!keyButton->isChecked()) {
        zaro::app::testing::failf("the panel does not show a keyframe at the playhead\n");
    }

    // A second keyframe, made by typing a value at another position.
    window.setPosition(zaro::time::RationalTime{40, sequence.frameRate()});
    QApplication::processEvents();
    if (keyButton->isChecked()) {
        zaro::app::testing::failf("the panel claims a keyframe where there is none\n");
    }
    auto* opacitySpin = window.effects()
                            ->findChild<QToolButton*>("keyframe:opacity")
                            ->parentWidget()
                            ->findChild<QDoubleSpinBox*>();
    opacitySpin->setValue(0.2);
    QApplication::processEvents();

    curve = clipNow()->animation.find(zaro::model::Param::Opacity);
    if (curve == nullptr || curve->size() != 2) {
        zaro::app::testing::failf("typing a value while animated did not add a keyframe\n");
    }
    std::printf("  stopwatch and a typed value made %zu keyframes\n", curve->size());

    const std::int64_t drawn = lanePixels();
    std::printf("  keyframe diamonds cover %lld pixels in the lane (%lld before)\n",
                static_cast<long long>(drawn), static_cast<long long>(bareLane));
    if (bareLane != 0) {
        zaro::app::testing::failf(
            "something else is painting in the lane, so this check "
            "proves nothing\n");
    }
    if (drawn < 20) {
        zaro::app::testing::failf("the keyframes are not drawn on the timeline\n");
    }
    if (drawn < 20) {
        zaro::app::testing::failf("the keyframes are not drawn on the timeline\n");
    }

    // Drag the second diamond earlier, through the timeline.
    const auto keyRow = timeline->rowFor(videoTrack.id());
    const int laneY = keyRow->top + keyRow->height - 3;
    const int fromX = static_cast<int>(
        timeline->layout().xForTime(zaro::time::RationalTime{40, sequence.frameRate()}));
    const int toX = static_cast<int>(
        timeline->layout().xForTime(zaro::time::RationalTime{30, sequence.frameRate()}));
    dragOnTimeline(timeline, fromX, toX, laneY);
    QApplication::processEvents();

    curve = clipNow()->animation.find(zaro::model::Param::Opacity);
    // Where the pointer actually was, not where it was aimed: a frame
    // is wider than a pixel is precise, and asking for frame 30 by
    // pixel can legitimately land on 29.
    const zaro::time::RationalTime moved =
        clipNow()->sourceTimeAt(timeline->layout().timeForX(toX, sequence.frameRate()));
    if (curve == nullptr || curve->size() != 2 || curve->at(moved) == nullptr) {
        zaro::app::testing::failf("the keyframe did not follow the drag\n");
    }
    std::printf("  dragged a keyframe to source frame %lld, value %.2f\n",
                static_cast<long long>(moved.frames()), curve->at(moved)->value);

    // Alt-click deletes one. The *first* keyframe, not the one just
    // dragged: it holds the same value as the static opacity, so
    // deleting it is what leaves the two different and makes the
    // stopwatch-off check below able to fail.
    {
        const int firstX = static_cast<int>(
            timeline->layout().xForTime(zaro::time::RationalTime{10, sequence.frameRate()}));
        const QPointF where(firstX, laneY);
        QMouseEvent press(QEvent::MouseButtonPress, where, where, Qt::LeftButton, Qt::LeftButton,
                          Qt::AltModifier);
        QCoreApplication::sendEvent(timeline, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, where, where, Qt::LeftButton, Qt::NoButton,
                            Qt::AltModifier);
        QCoreApplication::sendEvent(timeline, &release);
    }
    QApplication::processEvents();
    curve = clipNow()->animation.find(zaro::model::Param::Opacity);
    if (curve == nullptr || curve->size() != 1) {
        zaro::app::testing::failf("alt-click did not delete the keyframe\n");
    }

    // And the stopwatch off again, keeping what was on screen.
    window.setPosition(zaro::time::RationalTime{10, sequence.frameRate()});
    QApplication::processEvents();
    const double showing =
        clipNow()->transformAt(zaro::time::RationalTime{10, sequence.frameRate()}).opacity;
    stopwatch->click();
    QApplication::processEvents();
    if (!clipNow()->animation.empty()) {
        zaro::app::testing::failf("the stopwatch did not stop animating\n");
    }
    if (std::fabs(showing - 1.0) < 1e-6) {
        zaro::app::testing::failf(
            "the animated value equals the static one, so this check "
            "cannot tell them apart\n");
    }
    if (std::fabs(clipNow()->transform.opacity - showing) > 1e-6) {
        zaro::app::testing::failf("turning animation off changed the picture (%.3f -> %.3f)\n",
                                  showing, clipNow()->transform.opacity);
    }
    std::printf("  stopwatch off kept opacity at %.2f\n", clipNow()->transform.opacity);

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
}

// The inspector's sliders: that the knob says what the value says, and that
// moving it is an edit.
//
// The panel's own connections are what this is about. A slider that looked
// right and wrote nothing, or one that wrote correctly and sat at the wrong
// end, are both bugs that no headless test of the operations can see -- and
// both were in the first version of this row.
TEST_CASE("The inspector's parameter sliders", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    const auto& sequence = *window.sequence();
    const auto& videoTrack = sequence.videoTracks().front();
    const auto clipId = videoTrack.clips().front().id;

    auto* slider = window.effects()->findChild<QSlider*>("slider:opacity");
    auto* spin = window.effects()->findChild<QDoubleSpinBox*>("spin:opacity");
    if (slider == nullptr || spin == nullptr) {
        zaro::app::testing::failf("the opacity row has no slider\n");
    }

    window.effects()->setSelection(videoTrack.id(), clipId);
    window.setPosition(zaro::time::RationalTime{10, sequence.frameRate()});
    QApplication::processEvents();

    const auto clipNow = [&]() -> const zaro::model::Clip* {
        return window.sequence()->videoTracks().front().find(clipId);
    };

    // Opacity spans 0..1 and starts at 1, so a knob that follows its value is
    // at the top of its travel. The first version of this row left it at the
    // bottom, because a spin box set to the value it already holds emits
    // nothing for the slider to hear.
    if (slider->value() != slider->maximum()) {
        zaro::app::testing::failf("opacity is %.3f but its knob is at %d of %d\n",
                                  clipNow()->transform.opacity, slider->value(), slider->maximum());
    }

    // Halfway down. Set rather than dragged: what is being checked is the
    // panel's wiring, and QSlider's own mouse handling is Qt's to test.
    slider->setValue(slider->maximum() / 2);
    QApplication::processEvents();
    if (std::fabs(clipNow()->transform.opacity - 0.5) > 0.01) {
        zaro::app::testing::failf("moving the slider to the middle left opacity at %.3f\n",
                                  clipNow()->transform.opacity);
    }
    if (std::fabs(spin->value() - clipNow()->transform.opacity) > 1e-6) {
        zaro::app::testing::failf("the slider and the value box disagree: %.3f and %.3f\n",
                                  spin->value(), clipNow()->transform.opacity);
    }

    // And back the other way: a value written to the box moves the knob.
    spin->setValue(0.25);
    QApplication::processEvents();
    if (std::fabs(clipNow()->transform.opacity - 0.25) > 1e-6) {
        zaro::app::testing::failf("typing 0.25 left opacity at %.3f\n",
                                  clipNow()->transform.opacity);
    }
    const int quarter = slider->maximum() / 4;
    if (std::abs(slider->value() - quarter) > 2) {
        zaro::app::testing::failf("the knob is at %d, not near %d\n", slider->value(), quarter);
    }

    // Undo puts both halves of the row back, because the panel re-reads the
    // clip rather than trusting its own widgets.
    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.effects()->refresh();
    QApplication::processEvents();
    if (std::fabs(clipNow()->transform.opacity - 1.0) > 1e-6 ||
        slider->value() != slider->maximum()) {
        zaro::app::testing::failf("after undo opacity is %.3f with its knob at %d\n",
                                  clipNow()->transform.opacity, slider->value());
    }
    std::printf("  slider, value box and model agree through an edit and an undo\n");
}

// The inspector's header: three tabs, and what each one is for.
//
// The panel used to be one column somebody scrolled: motion, colour, a mask, a
// secondary, effects and then audio, with whichever half did not apply hidden.
// The tabs make that a choice rather than a scroll, so what has to hold is that
// the choice sticks, that a tab with nothing behind it says so, and that
// selecting a clip lands on a page with something on it.
TEST_CASE("The inspector's tabs", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    auto* panel = window.effects();
    const auto& sequence = *window.sequence();
    const auto& videoTrack = sequence.videoTracks().front();
    const auto videoClip = videoTrack.clips().front().id;

    auto* inspectorTab = panel->findChild<QPushButton*>("inspector-tab-inspector");
    auto* audioTab = panel->findChild<QPushButton*>("inspector-tab-audio");
    auto* infoTab = panel->findChild<QPushButton*>("inspector-tab-info");
    if (inspectorTab == nullptr || audioTab == nullptr || infoTab == nullptr) {
        zaro::app::testing::failf("the inspector has no tab strip\n");
    }

    panel->setSelection(videoTrack.id(), videoClip);
    QApplication::processEvents();

    // A picture clip lands on Inspector, and the Audio tab says there is
    // nothing behind it rather than showing an empty page.
    if (panel->pane() != zaro::app::EffectControls::Pane::Inspector || !inspectorTab->isChecked()) {
        zaro::app::testing::failf("a video clip did not land on the Inspector tab\n");
    }
    if (audioTab->isEnabled()) {
        zaro::app::testing::failf("the Audio tab is offered for a clip with no sound\n");
    }
    if (!infoTab->isEnabled()) {
        zaro::app::testing::failf("the Info tab is not offered for a selected clip\n");
    }

    // Motion is on the Inspector page and nowhere else.
    auto* motionSlider = panel->findChild<QSlider*>("slider:opacity");
    if (motionSlider == nullptr || !motionSlider->isVisibleTo(panel)) {
        zaro::app::testing::failf("the Inspector page is not showing motion\n");
    }

    infoTab->click();
    QApplication::processEvents();
    if (panel->pane() != zaro::app::EffectControls::Pane::Info) {
        zaro::app::testing::failf("clicking Info did not change the page\n");
    }
    if (motionSlider->isVisibleTo(panel)) {
        zaro::app::testing::failf("motion is still showing on the Info page\n");
    }
    // The Info page answers with the clip's own name, which is how a reader
    // knows the page is about the selection and not about the last one.
    const QString name =
        QString::fromStdString(window.sequence()->videoTracks().front().clips().front().name);
    bool named = false;
    for (auto* value : panel->findChildren<QLabel*>("inspector-info-value")) {
        named = named || value->text() == name;
    }
    if (!named) {
        zaro::app::testing::failf("the Info page does not name the clip (%s)\n",
                                  name.toUtf8().constData());
    }

    // Selecting a sound clip while Info is up stays on Info. The panel moves
    // somebody off a page only when that page has nothing on it, and Info
    // answers for every clip -- switching away would be overriding a choice
    // that is still valid.
    const auto& audioTrack = window.sequence()->audioTracks().front();
    if (audioTrack.clips().empty()) {
        zaro::app::testing::failf("the fixture has no audio clip to select\n");
    }
    panel->setSelection(audioTrack.id(), audioTrack.clips().front().id);
    QApplication::processEvents();
    if (panel->pane() != zaro::app::EffectControls::Pane::Info) {
        zaro::app::testing::failf("changing the selection moved off the Info page\n");
    }
    if (inspectorTab->isEnabled()) {
        zaro::app::testing::failf("the Inspector tab is offered for a clip with no picture\n");
    }
    if (!audioTab->isEnabled()) {
        zaro::app::testing::failf("the Audio tab is not offered for a sound clip\n");
    }

    audioTab->click();
    QApplication::processEvents();
    auto* gain = panel->findChild<QSlider*>("slider:gainDb");
    if (gain == nullptr || !gain->isVisibleTo(panel)) {
        zaro::app::testing::failf("the Audio page is not showing the level\n");
    }

    // And back the other way: the Audio page has nothing on it for a picture
    // clip, so this one *is* moved, to the page that has.
    panel->setSelection(videoTrack.id(), videoClip);
    QApplication::processEvents();
    if (panel->pane() != zaro::app::EffectControls::Pane::Inspector) {
        zaro::app::testing::failf("a video clip was left on the empty Audio page\n");
    }

    // And the reset arrow puts the page it is over back, undoably.
    auto* opacity = panel->findChild<QDoubleSpinBox*>("spin:opacity");
    opacity->setValue(0.4);
    QApplication::processEvents();
    const auto clipNow = [&]() -> const zaro::model::Clip* {
        return window.sequence()->videoTracks().front().find(videoClip);
    };
    if (std::fabs(clipNow()->transform.opacity - 0.4) > 1e-6) {
        zaro::app::testing::failf("the panel did not write 0.4\n");
    }
    for (auto* button : panel->findChildren<QPushButton*>("bin-glyph-button")) {
        if (button->parentWidget() == panel->findChild<QWidget*>("inspector-tabbar")) {
            button->click();
        }
    }
    QApplication::processEvents();
    if (std::fabs(clipNow()->transform.opacity - 1.0) > 1e-6) {
        zaro::app::testing::failf("reset left opacity at %.3f\n", clipNow()->transform.opacity);
    }
    window.commands().undo(window.project());
    panel->refresh();
    QApplication::processEvents();
    if (std::fabs(clipNow()->transform.opacity - 0.4) > 1e-6) {
        zaro::app::testing::failf("undo did not take the reset back (%.3f)\n",
                                  clipNow()->transform.opacity);
    }
    std::printf("  tabs: Inspector, Audio and Info, each offered only where it applies\n");

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
}

// What the inspector shows depends on what the clip *is*, not on which track
// it happens to sit on.
//
// The panel used to answer this with the track kind alone, so a title, a
// rectangle and an adjustment layer all got the panel a camera clip gets --
// including a keyer, which cannot say anything true about a generated picture,
// and a position, which an adjustment layer does not have. See
// `model::clipKindOf` and `groupsFor` in EffectControls.cpp.
TEST_CASE("The inspector shows what the kind of clip has", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    auto* panel = window.effects();
    const auto& sequence = *window.sequence();
    const auto& videoTrack = sequence.videoTracks().front();
    const auto sequenceId = sequence.id();
    const auto rate = sequence.frameRate();

    const auto group = [&](const char* name) {
        auto* box = panel->findChild<QGroupBox*>(name);
        if (box == nullptr) {
            zaro::app::testing::failf("the inspector has no %s\n", name);
        }
        return box;
    };
    const auto shown = [&](const char* name) { return group(name)->isVisibleTo(panel); };

    // --- a clip that reads a file -------------------------------------------
    panel->setSelection(videoTrack.id(), videoTrack.clips().front().id);
    QApplication::processEvents();
    if (!shown("inspector-group-key") || !shown("inspector-group-secondary")) {
        zaro::app::testing::failf("a camera clip is missing its key or its secondary\n");
    }
    if (shown("inspector-group-shape") || shown("inspector-group-text")) {
        zaro::app::testing::failf("a camera clip is offered controls for a generated picture\n");
    }
    auto* positionRow = panel->findChild<QSlider*>("slider:positionX");
    if (positionRow == nullptr || !positionRow->isVisibleTo(panel)) {
        zaro::app::testing::failf("a camera clip cannot be positioned\n");
    }

    // --- a generated shape ---------------------------------------------------
    const auto& videoTracks = window.project().findSequence(sequenceId)->videoTracks();
    const auto topTrack = videoTracks.size() > 1 ? videoTracks[1].id() : videoTrack.id();
    const zaro::time::TimeRange span{zaro::time::RationalTime{0, rate},
                                     zaro::time::RationalTime{40, rate}};

    zaro::model::Graphic rectangle;
    rectangle.kind = zaro::model::GraphicKind::Rectangle;
    auto placed =
        zaro::edit::makeAddGraphic(window.project(), {sequenceId, topTrack}, rectangle, span);
    if (!placed) {
        zaro::app::testing::failf("%s\n", placed.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*placed));
    const auto shapeClip =
        window.project().findSequence(sequenceId)->findTrack(topTrack)->clips().front().id;
    panel->setSelection(topTrack, shapeClip);
    QApplication::processEvents();

    if (!shown("inspector-group-shape")) {
        zaro::app::testing::failf("a shape has no shape controls\n");
    }
    // A keyer measures what a camera saw. A rectangle is already exactly as
    // transparent as it was authored to be, and a qualifier over a flat fill
    // selects all of it or none of it.
    if (shown("inspector-group-key") || shown("inspector-group-secondary")) {
        zaro::app::testing::failf("a shape is offered a keyer or a secondary\n");
    }
    if (shown("inspector-group-text")) {
        zaro::app::testing::failf("a rectangle is offered typography\n");
    }
    window.commands().undo(window.project());

    // --- a title -------------------------------------------------------------
    zaro::model::Graphic title;
    title.kind = zaro::model::GraphicKind::Text;
    title.text = "Chapter one";
    title.width = 800.0;
    auto placedTitle =
        zaro::edit::makeAddGraphic(window.project(), {sequenceId, topTrack}, title, span);
    if (!placedTitle) {
        zaro::app::testing::failf("%s\n", placedTitle.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*placedTitle));
    const auto titleClip =
        window.project().findSequence(sequenceId)->findTrack(topTrack)->clips().front().id;
    panel->setSelection(topTrack, titleClip);
    QApplication::processEvents();

    if (!shown("inspector-group-text")) {
        zaro::app::testing::failf("a title has no text controls\n");
    }
    auto* body = panel->findChild<QPlainTextEdit*>("text-body");
    if (body == nullptr || body->toPlainText() != QString{"Chapter one"}) {
        zaro::app::testing::failf("the panel is not showing what the title says\n");
    }
    // The Kind list is for choosing between a rectangle and an ellipse, which
    // is not a choice a title has.
    auto* kindList = panel->findChild<QComboBox*>("shape-kind");
    if (kindList == nullptr || kindList->isVisibleTo(panel)) {
        zaro::app::testing::failf("a title is offered a shape kind to pick\n");
    }

    const auto titleNow = [&]() -> const zaro::model::Clip* {
        return window.project().findSequence(sequenceId)->findTrack(topTrack)->find(titleClip);
    };

    // The bug this guards: the Kind list had no entry for Text, so selecting a
    // title left it on index -1, and the next edit to any other field read that
    // back as GraphicKind::None -- which unset the graphic and deleted the
    // title's picture.
    auto* width = panel->findChild<QDoubleSpinBox*>("spin-shape-width");
    if (width == nullptr) {
        // Named through its label rather than an object name; find it by value.
        for (auto* candidate : panel->findChildren<QDoubleSpinBox*>()) {
            if (std::fabs(candidate->value() - 800.0) < 1e-6) {
                width = candidate;
            }
        }
    }
    if (width == nullptr) {
        zaro::app::testing::failf("the title's box has no width field\n");
    }
    width->setValue(900.0);
    QApplication::processEvents();
    if (titleNow() == nullptr || titleNow()->graphic.kind != zaro::model::GraphicKind::Text) {
        zaro::app::testing::failf("editing the box turned the title into a %s\n",
                                  titleNow() == nullptr
                                      ? "deleted clip"
                                      : zaro::model::toString(titleNow()->graphic.kind));
    }
    if (titleNow()->graphic.text != "Chapter one") {
        zaro::app::testing::failf("editing the box lost the words\n");
    }
    if (std::fabs(titleNow()->graphic.width - 900.0) > 1e-6) {
        zaro::app::testing::failf("the width did not reach the model\n");
    }

    // Typing reaches the model when the field is done being typed in, not per
    // keystroke.
    body->setPlainText("Chapter two");
    // The focus-out itself, sent rather than provoked. Asking for the focus and
    // then giving it up depends on the window being the active one, which it is
    // not when the whole suite shares one and something else ran last -- and a
    // test that only passes when it runs alone is worse than no test.
    QFocusEvent away(QEvent::FocusOut);
    QCoreApplication::sendEvent(body, &away);
    QApplication::processEvents();
    if (titleNow()->graphic.text != "Chapter two") {
        zaro::app::testing::failf("the typed title did not reach the model (%s)\n",
                                  titleNow()->graphic.text.c_str());
    }

    // And typing that is never given up: clicking a different clip on the
    // timeline changes the selection, and the words typed into the one being
    // left have to survive that.
    body->setPlainText("Chapter three");
    panel->setSelection(videoTrack.id(), videoTrack.clips().front().id);
    QApplication::processEvents();
    if (titleNow()->graphic.text != "Chapter three") {
        zaro::app::testing::failf("changing the selection lost the typed title (%s)\n",
                                  titleNow()->graphic.text.c_str());
    }
    window.commands().undo(window.project());
    window.commands().undo(window.project());

    // --- an adjustment layer --------------------------------------------------
    auto adjustment = zaro::edit::makeAddAdjustment(window.project(), {sequenceId, topTrack}, span);
    if (!adjustment) {
        zaro::app::testing::failf("%s\n", adjustment.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*adjustment));
    const auto adjustmentClip =
        window.project().findSequence(sequenceId)->findTrack(topTrack)->clips().front().id;
    panel->setSelection(topTrack, adjustmentClip);
    QApplication::processEvents();

    // It grades what is under it, so it keeps the grade and the mask that
    // limits it -- and loses the position, because it has no picture of its own
    // to move.
    if (!shown("inspector-group-colour") || !shown("inspector-group-mask") ||
        !shown("inspector-group-secondary")) {
        zaro::app::testing::failf("an adjustment layer lost the controls it grades with\n");
    }
    if (positionRow->isVisibleTo(panel)) {
        zaro::app::testing::failf("an adjustment layer is offered a position\n");
    }
    auto* opacityRow = panel->findChild<QSlider*>("slider:opacity");
    if (opacityRow == nullptr || !opacityRow->isVisibleTo(panel)) {
        zaro::app::testing::failf("an adjustment layer has no opacity\n");
    }
    if (shown("inspector-group-key")) {
        zaro::app::testing::failf("an adjustment layer is offered a keyer\n");
    }

    std::printf("  inspector: camera, shape, title and adjustment each shown what they have\n");

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    panel->setSelection(videoTrack.id(), videoTrack.clips().front().id);
    QApplication::processEvents();
}

// Speed and direction, which the model has always been able to express and the
// panel had no way to ask for. The number is derived rather than stored -- it
// is the ratio between what a clip reads and how long it takes to read it -- so
// what this checks is that the panel writes through the retiming operation and
// reads the answer back off the clip.
TEST_CASE("Speed and reverse, set from the inspector", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    auto* panel = window.effects();
    const auto& sequence = *window.sequence();
    const auto sequenceId = sequence.id();
    const auto trackId = sequence.videoTracks().front().id();
    const auto clipId = sequence.videoTracks().front().clips().front().id;
    const auto framesBefore =
        sequence.videoTracks().front().clips().front().timelineRange.duration().frames();

    panel->setSelection(trackId, clipId);
    QApplication::processEvents();

    auto* speed = panel->findChild<QDoubleSpinBox*>("clip-speed");
    auto* reverse = panel->findChild<QCheckBox*>("clip-reverse");
    if (speed == nullptr || reverse == nullptr) {
        zaro::app::testing::failf("the inspector has no speed control\n");
    }
    if (std::fabs(speed->value() - 100.0) > 0.5) {
        zaro::app::testing::failf("an untouched clip reads as %.1f%%\n", speed->value());
    }

    const auto clipNow = [&]() -> const zaro::model::Clip* {
        return window.project().findSequence(sequenceId)->findTrack(trackId)->find(clipId);
    };

    speed->setValue(200.0);
    QApplication::processEvents();
    // Twice as fast is half as long. Within a frame: a retime has to land on a
    // whole number of frames, and an odd-length clip cannot halve exactly.
    const auto framesAfter = clipNow()->timelineRange.duration().frames();
    if (std::llabs(framesAfter - framesBefore / 2) > 1) {
        zaro::app::testing::failf("at 200%% the clip is %lld frames, was %lld\n",
                                  static_cast<long long>(framesAfter),
                                  static_cast<long long>(framesBefore));
    }
    if (clipNow()->speed() < 1.9 || clipNow()->speed() > 2.1) {
        zaro::app::testing::failf("the clip reads back at %.3fx\n", clipNow()->speed());
    }

    reverse->setChecked(true);
    QApplication::processEvents();
    if (!clipNow()->reversed) {
        zaro::app::testing::failf("the reverse switch did not reach the clip\n");
    }

    window.commands().undo(window.project());
    panel->refresh();
    QApplication::processEvents();
    if (clipNow()->reversed) {
        zaro::app::testing::failf("undo did not take the reverse back\n");
    }

    // And a generated picture has no speed to set: there is no source being
    // read faster or slower, only a shape that lasts as long as it lasts.
    const auto& videoTracks = window.project().findSequence(sequenceId)->videoTracks();
    const auto topTrack = videoTracks.size() > 1 ? videoTracks[1].id() : trackId;
    zaro::model::Graphic rectangle;
    rectangle.kind = zaro::model::GraphicKind::Rectangle;
    auto placed = zaro::edit::makeAddGraphic(
        window.project(), {sequenceId, topTrack}, rectangle,
        zaro::time::TimeRange{zaro::time::RationalTime{0, sequence.frameRate()},
                              zaro::time::RationalTime{40, sequence.frameRate()}});
    if (!placed) {
        zaro::app::testing::failf("%s\n", placed.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*placed));
    const auto shapeClip =
        window.project().findSequence(sequenceId)->findTrack(topTrack)->clips().front().id;
    panel->setSelection(topTrack, shapeClip);
    QApplication::processEvents();
    if (speed->isVisibleTo(panel)) {
        zaro::app::testing::failf("a generated shape is offered a playback speed\n");
    }

    std::printf("  speed: 100%% to 200%% halved the clip, and reversed and back\n");

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    panel->setSelection(trackId, clipId);
    QApplication::processEvents();
}

// The two kinds of clip whose defining property the panel could not reach: a
// multicam clip's cameras, and the sequence a nested clip is made of.
//
// `makeSetAngleOffsets` in particular had no caller anywhere in the program --
// angles could be synced by the command that placed them and never afterwards.
TEST_CASE("Angles and nested sequences, from the inspector", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    auto* panel = window.effects();
    const auto& sequence = *window.sequence();
    const auto sequenceId = sequence.id();
    const auto rate = sequence.frameRate();
    const auto trackId = sequence.videoTracks().front().id();
    const auto mediaId = window.project().media().front().id;

    const auto group = [&](const char* name) {
        auto* box = panel->findChild<QGroupBox*>(name);
        if (box == nullptr) {
            zaro::app::testing::failf("the inspector has no %s\n", name);
        }
        return box;
    };

    // --- a multicam clip ------------------------------------------------------
    zaro::model::Clip::Angle a;
    a.media = mediaId;
    a.offset = zaro::time::RationalTime{0, rate};
    a.name = "Camera A";
    zaro::model::Clip::Angle b;
    b.media = mediaId;
    b.offset = zaro::time::RationalTime{0, rate};
    b.name = "Camera B";

    auto built =
        zaro::edit::makeMulticam(window.project(), {sequenceId, trackId}, {a, b},
                                 zaro::time::TimeRange{zaro::time::RationalTime{0, rate},
                                                       zaro::time::RationalTime{60, rate}});
    if (!built) {
        zaro::app::testing::failf("%s\n", built.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*built));
    const auto multicamClip =
        window.project().findSequence(sequenceId)->findTrack(trackId)->clips().front().id;

    window.setPosition(zaro::time::RationalTime{25, rate});
    panel->setSelection(trackId, multicamClip);
    QApplication::processEvents();

    if (!group("inspector-group-angles")->isVisibleTo(panel)) {
        zaro::app::testing::failf("a multicam clip is not shown its cameras\n");
    }
    auto* list = panel->findChild<QListWidget*>("angle-list");
    auto* offset = panel->findChild<QDoubleSpinBox*>("angle-offset");
    auto* cut = panel->findChild<QPushButton*>("angle-switch");
    if (list == nullptr || offset == nullptr || cut == nullptr) {
        zaro::app::testing::failf("the angles group is missing its controls\n");
    }
    if (list->count() != 2) {
        zaro::app::testing::failf("the panel lists %d cameras, not 2\n", list->count());
    }
    // The live one is marked in the text, so a row can say what the clip is
    // doing and what the selection is about to do at the same time.
    if (!list->item(0)->text().contains("Camera A") ||
        !list->item(0)->text().contains(QString::fromUtf8("●"))) {
        zaro::app::testing::failf("the live camera is not marked (%s)\n",
                                  list->item(0)->text().toUtf8().constData());
    }

    const auto clipNow = [&]() -> const zaro::model::Clip* {
        return window.project().findSequence(sequenceId)->findTrack(trackId)->find(multicamClip);
    };

    // Syncing an angle. Nothing else in the program can do this.
    list->setCurrentRow(1);
    QApplication::processEvents();
    offset->setValue(-2.0);
    QApplication::processEvents();
    const double synced = clipNow()->angles[1].offset.toSecondsDouble();
    if (std::fabs(synced + 2.0) > 0.05) {
        zaro::app::testing::failf("camera B synced to %+.3fs, not -2s\n", synced);
    }
    if (std::fabs(clipNow()->angles[0].offset.toSecondsDouble()) > 1e-9) {
        zaro::app::testing::failf("syncing one camera moved the other\n");
    }

    // Cutting to the other angle. A switch is a cut: the clip is split and the
    // part after it takes the new camera.
    const auto clipsOn = [&]() {
        return window.project().findSequence(sequenceId)->findTrack(trackId)->clips().size();
    };
    const std::size_t before = clipsOn();
    if (!cut->isEnabled()) {
        zaro::app::testing::failf("the panel will not cut to the camera that is not live\n");
    }
    cut->click();
    QApplication::processEvents();
    if (clipsOn() != before + 1) {
        zaro::app::testing::failf("cutting to an angle did not split the clip (%zu then %zu)\n",
                                  before, clipsOn());
    }
    const auto& clips = window.project().findSequence(sequenceId)->findTrack(trackId)->clips();
    if (clips[1].activeAngle != 1 || clips[1].start().frames() != 25) {
        zaro::app::testing::failf("the cut landed at %lld on angle %d\n",
                                  static_cast<long long>(clips[1].start().frames()),
                                  clips[1].activeAngle);
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    panel->setSelection(
        trackId, window.project().findSequence(sequenceId)->findTrack(trackId)->clips().front().id);
    QApplication::processEvents();

    // --- a nested sequence ----------------------------------------------------
    // A second sequence to nest. Made here rather than looked for, because the
    // fixture project has one and nesting a sequence inside itself is refused
    // -- rightly, since it is a render that never finishes.
    zaro::model::SequenceId inner;
    {
        zaro::model::Sequence made{window.project().ids().next<zaro::model::SequenceTag>(),
                                   "Insert", rate};
        made.setSize(1920, 1080);
        inner = made.id();
        const auto innerTrack = window.project().ids().next<zaro::model::TrackTag>();
        made.addTrack(innerTrack, zaro::model::TrackKind::Video, "V1");
        // With something in it: nesting an empty sequence is refused, and
        // rightly -- it would composite to nothing.
        zaro::model::Clip content;
        content.id = window.project().ids().next<zaro::model::ClipTag>();
        content.source = mediaId;
        content.name = "inside";
        content.sourceRange = zaro::time::TimeRange{zaro::time::RationalTime{0, rate},
                                                    zaro::time::RationalTime{40, rate}};
        content.timelineRange = content.sourceRange;
        made.findTrack(innerTrack)->insert(std::move(content));
        window.project().addSequence(std::move(made));
    }
    const auto& videoTracks = window.project().findSequence(sequenceId)->videoTracks();
    const auto topTrack = videoTracks.size() > 1 ? videoTracks[1].id() : trackId;
    auto nested = zaro::edit::makeNestSequence(window.project(), {sequenceId, topTrack}, inner,
                                               zaro::time::RationalTime{0, rate});
    if (!nested) {
        zaro::app::testing::failf("%s\n", nested.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*nested));
    const auto nestedClip =
        window.project().findSequence(sequenceId)->findTrack(topTrack)->clips().front().id;
    panel->setSelection(topTrack, nestedClip);
    QApplication::processEvents();

    if (!group("inspector-group-nested")->isVisibleTo(panel)) {
        zaro::app::testing::failf("a nested clip has no way into its sequence\n");
    }
    // It has no file, so there is nothing to stabilise and nothing to key --
    // but it is composited pixels, so the grade stays.
    if (group("inspector-group-angles")->isVisibleTo(panel)) {
        zaro::app::testing::failf("a nested clip is offered cameras\n");
    }
    auto* open = panel->findChild<QPushButton*>("nested-open");
    if (open == nullptr || !open->isEnabled()) {
        zaro::app::testing::failf("the way into the nested sequence is not offered\n");
    }
    open->click();
    QApplication::processEvents();
    if (window.sequence()->id() != inner) {
        zaro::app::testing::failf("opening the nested sequence did not change the window\n");
    }

    std::printf("  angles: 2 cameras listed, one synced to -2s, one cut to; nested opened\n");

    window.setActiveSequence(sequenceId);
    QApplication::processEvents();
    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    QApplication::processEvents();
}

// Several clips at once. The timeline has supported selecting more than one
// since 4j; the inspector showed the first and wrote to the first.
//
// The rule is that what stays on screen is what can be written to all of them.
// A mask, a grade chain, an effect stack: each is a piece of work on one clip,
// and showing the primary's while the header says "3 clips" means every edit
// lands on a clip whose name is not the one on screen.
TEST_CASE("The inspector edits every selected clip", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    auto* panel = window.effects();
    auto* timeline = window.timeline();
    const auto& sequence = *window.sequence();
    const auto sequenceId = sequence.id();
    const auto rate = sequence.frameRate();
    const auto trackId = sequence.videoTracks().front().id();

    // Three clips on one track, so a rubber band would have caught all three.
    auto place = [&](std::int64_t start) {
        auto built = zaro::edit::makeOverwrite(window.project(), {sequenceId, trackId}, [&] {
            zaro::model::Clip c;
            c.id = window.project().ids().next<zaro::model::ClipTag>();
            c.source = window.project().media().front().id;
            c.name = "take";
            c.sourceRange = zaro::time::TimeRange{zaro::time::RationalTime{0, rate},
                                                  zaro::time::RationalTime{20, rate}};
            c.timelineRange = zaro::time::TimeRange{zaro::time::RationalTime{start, rate},
                                                    zaro::time::RationalTime{20, rate}};
            return c;
        }());
        if (!built) {
            zaro::app::testing::failf("%s\n", built.error().toString().c_str());
        }
        window.commands().execute(window.project(), std::move(*built));
    };
    place(400);
    place(430);
    place(460);

    const auto& clips = window.project().findSequence(sequenceId)->findTrack(trackId)->clips();
    std::vector<zaro::edit::ClipRef> picked;
    for (const auto& clip : clips) {
        if (clip.start().frames() >= 400) {
            picked.push_back(zaro::edit::ClipRef{trackId, clip.id});
        }
    }
    if (picked.size() != 3) {
        zaro::app::testing::failf("expected 3 clips to select, found %zu\n", picked.size());
    }

    // Through the timeline, so the wiring between the two is what is tested and
    // not just the panel's own entry point.
    timeline->selectOnly(picked[0].track, picked[0].clip);
    for (std::size_t i = 1; i < picked.size(); ++i) {
        timeline->selectAlso(picked[i].track, picked[i].clip);
    }
    QApplication::processEvents();

    const auto opacityOf = [&](std::size_t which) {
        const zaro::model::Clip* clip =
            window.project().findSequence(sequenceId)->findTrack(trackId)->find(picked[which].clip);
        return clip == nullptr ? -1.0 : clip->transform.opacity;
    };

    const std::size_t stepsBefore = window.commands().depth();
    auto* opacity = panel->findChild<QDoubleSpinBox*>("spin:opacity");
    if (opacity == nullptr) {
        zaro::app::testing::failf("no opacity field\n");
    }
    opacity->setValue(0.4);
    QApplication::processEvents();

    for (std::size_t i = 0; i < picked.size(); ++i) {
        if (std::fabs(opacityOf(i) - 0.4) > 1e-6) {
            zaro::app::testing::failf("clip %zu is at %.3f, not 0.4\n", i, opacityOf(i));
        }
    }
    // One step, not three: setting three clips to 40% is one gesture, and
    // undoing it a clip at a time is not what anybody meant by it.
    if (window.commands().depth() != stepsBefore + 1) {
        zaro::app::testing::failf("three clips took %zu undo steps\n",
                                  window.commands().depth() - stepsBefore);
    }
    window.commands().undo(window.project());
    panel->refresh();
    QApplication::processEvents();
    for (std::size_t i = 0; i < picked.size(); ++i) {
        if (std::fabs(opacityOf(i) - 1.0) > 1e-6) {
            zaro::app::testing::failf("one undo left clip %zu at %.3f\n", i, opacityOf(i));
        }
    }

    // What is on screen is what can be written to all of them. A mask is not.
    auto* mask = panel->findChild<QGroupBox*>("inspector-group-mask");
    auto* effects = panel->findChild<QGroupBox*>("inspector-group-effects");
    if (mask == nullptr || effects == nullptr) {
        zaro::app::testing::failf("the inspector is missing a group\n");
    }
    if (mask->isVisibleTo(panel) || effects->isVisibleTo(panel)) {
        zaro::app::testing::failf("a multi-selection is offered a mask or an effect stack\n");
    }
    // The same rule inside the Colour group, where the five parameter rows are
    // values and everything under them -- a curve somebody drew, a LUT they
    // loaded, a set of wheels they balanced -- is one clip's own work.
    auto* lutAmount = panel->findChild<QDoubleSpinBox*>("lut-amount");
    auto* wheel = panel->findChild<QDoubleSpinBox*>("wheel-0-0");
    auto* vignette = panel->findChild<QDoubleSpinBox*>("vignette-amount");
    if (lutAmount == nullptr || wheel == nullptr || vignette == nullptr) {
        zaro::app::testing::failf("the colour group is missing a control\n");
    }
    if (lutAmount->isVisibleTo(panel) || wheel->isVisibleTo(panel) ||
        vignette->isVisibleTo(panel)) {
        zaro::app::testing::failf("a multi-selection is offered a LUT, wheels or a vignette\n");
    }
    auto* saturation = panel->findChild<QDoubleSpinBox*>("spin:saturation");
    if (saturation == nullptr || !saturation->isVisibleTo(panel)) {
        zaro::app::testing::failf("a multi-selection has no saturation\n");
    }
    // And the parameters are.
    if (!opacity->isVisibleTo(panel)) {
        zaro::app::testing::failf("a multi-selection has no opacity\n");
    }

    // A row the clips disagree about says so rather than quietly showing one
    // of them.
    opacity->setValue(0.5);
    QApplication::processEvents();
    auto singleBuilt =
        zaro::edit::makeSetTransform(window.project(), {sequenceId, trackId}, picked[2].clip, [&] {
            zaro::model::Transform t;
            t.opacity = 0.9;
            return t;
        }());
    if (!singleBuilt) {
        zaro::app::testing::failf("could not make one clip differ\n");
    }
    window.commands().execute(window.project(), std::move(*singleBuilt));
    panel->refresh();
    QApplication::processEvents();

    bool marked = false;
    for (auto* label : panel->findChildren<QLabel*>()) {
        if (label->property("rowLabel").toString() == "Opacity") {
            marked = label->text().contains('*');
        }
    }
    if (!marked) {
        zaro::app::testing::failf("a row the clips disagree about is not marked\n");
    }

    // Back to one clip, and everything comes back.
    timeline->selectOnly(picked[0].track, picked[0].clip);
    QApplication::processEvents();
    if (!mask->isVisibleTo(panel)) {
        zaro::app::testing::failf("selecting one clip did not bring the mask back\n");
    }
    if (!wheel->isVisibleTo(panel) || !lutAmount->isVisibleTo(panel)) {
        zaro::app::testing::failf("selecting one clip did not bring the grade chain back\n");
    }

    std::printf("  multi-selection: 3 clips set to 40%% in one undo step\n");

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    QApplication::processEvents();
}

// A track is not a clip, and its page is not a cut-down clip page. Pressing a
// track's header away from the buttons on it picks the track; what the panel
// then shows is what the track *is*.
TEST_CASE("The inspector shows a track when its header is picked", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    auto* panel = window.effects();
    auto* timeline = window.timeline();
    const auto& sequence = *window.sequence();
    const auto sequenceId = sequence.id();
    const auto videoTrackId = sequence.videoTracks().front().id();
    const auto audioTrackId = sequence.audioTracks().front().id();

    // Start from a clip, so the switch away from one is what is tested.
    timeline->selectOnly(videoTrackId, sequence.videoTracks().front().clips().front().id);
    QApplication::processEvents();

    const auto row = timeline->rowFor(videoTrackId);
    if (!row) {
        zaro::app::testing::failf("no row for the video track\n");
    }
    // The left edge of the header, above the buttons, which sit to the right
    // and low in the row.
    const int headerX = 12;
    const int headerY = row->top + 6;
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(headerX, headerY),
                      QPointF(headerX, headerY), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(timeline, &press);
    QApplication::processEvents();

    auto* trackGroup = panel->findChild<QGroupBox*>("inspector-group-track");
    auto* motion = panel->findChild<QGroupBox*>("inspector-group-motion");
    if (trackGroup == nullptr || motion == nullptr) {
        zaro::app::testing::failf("the inspector is missing a group\n");
    }
    if (!trackGroup->isVisibleTo(panel)) {
        zaro::app::testing::failf("picking a track header did not show the track\n");
    }
    if (motion->isVisibleTo(panel)) {
        zaro::app::testing::failf("a track is offered a clip's motion\n");
    }

    const auto trackNow = [&](zaro::model::TrackId id) {
        return window.project().findSequence(sequenceId)->findTrack(id);
    };

    // Renaming, locking and hiding, all through the real operations.
    auto* name = panel->findChild<QLineEdit*>("track-name");
    auto* muted = panel->findChild<QCheckBox*>("track-muted");
    auto* locked = panel->findChild<QCheckBox*>("track-locked");
    if (name == nullptr || muted == nullptr || locked == nullptr) {
        zaro::app::testing::failf("the track group is missing its controls\n");
    }
    // A picture track's mute is not a word anybody can hear.
    if (muted->text() != QString{"Hidden"}) {
        zaro::app::testing::failf("a picture track's mute reads as \"%s\"\n",
                                  muted->text().toUtf8().constData());
    }
    name->setText("B-roll");
    emit name->editingFinished();
    QApplication::processEvents();
    if (trackNow(videoTrackId)->name() != "B-roll") {
        zaro::app::testing::failf("the rename did not reach the track (%s)\n",
                                  trackNow(videoTrackId)->name().c_str());
    }
    muted->setChecked(true);
    locked->setChecked(true);
    QApplication::processEvents();
    if (!trackNow(videoTrackId)->isMuted() || !trackNow(videoTrackId)->isLocked()) {
        zaro::app::testing::failf("hiding and locking did not reach the track\n");
    }

    // A picture track contributes nothing to the mix, so its Audio tab is dark.
    auto* audioTab = panel->findChild<QPushButton*>("inspector-tab-audio");
    if (audioTab == nullptr || audioTab->isEnabled()) {
        zaro::app::testing::failf("a picture track is offered an Audio page\n");
    }

    // A sound track's is not, and holds its level.
    const auto audioRow = timeline->rowFor(audioTrackId);
    if (!audioRow) {
        zaro::app::testing::failf("no row for the audio track\n");
    }
    const int audioY = audioRow->top + 6;
    QMouseEvent pressAudio(QEvent::MouseButtonPress, QPointF(headerX, audioY),
                           QPointF(headerX, audioY), Qt::LeftButton, Qt::LeftButton,
                           Qt::NoModifier);
    QCoreApplication::sendEvent(timeline, &pressAudio);
    QApplication::processEvents();
    if (!audioTab->isEnabled()) {
        zaro::app::testing::failf("a sound track has no Audio page\n");
    }
    audioTab->click();
    QApplication::processEvents();
    auto* gain = panel->findChild<QDoubleSpinBox*>("track-gain");
    if (gain == nullptr || !gain->isVisibleTo(panel)) {
        zaro::app::testing::failf("a sound track's level is not shown\n");
    }
    gain->setValue(-6.0);
    QApplication::processEvents();
    if (std::fabs(trackNow(audioTrackId)->gainDb() + 6.0) > 1e-6) {
        zaro::app::testing::failf("the track fader did not reach the track\n");
    }

    // And picking a clip puts the clip back: the two are exclusive.
    timeline->selectOnly(
        videoTrackId,
        window.project().findSequence(sequenceId)->findTrack(videoTrackId)->clips().front().id);
    QApplication::processEvents();
    if (trackGroup->isVisibleTo(panel)) {
        zaro::app::testing::failf("picking a clip left the track page up\n");
    }

    std::printf("  track page: renamed, hidden, locked, and a fader at -6 dB\n");

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    QApplication::processEvents();
}

// The hue curve, drawn the way a person draws one. The table and the two render
// paths are checked elsewhere; what neither of those can see is whether picking
// "Hue vs Sat" and dragging in the widget reaches the clip.
TEST_CASE("The colour curves, driven with the mouse", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    const auto& sequence = *window.sequence();
    const auto sequenceId = sequence.id();
    const auto trackId = sequence.videoTracks().front().id();
    const auto clipId = sequence.videoTracks().front().clips().front().id;

    auto* editor = window.effects()->findChild<app::CurveEditor*>();
    auto* chooser = window.effects()->findChild<QComboBox*>("curve-channel");
    if (editor == nullptr || chooser == nullptr) {
        zaro::app::testing::failf("there is no curve editor\n");
    }
    window.effects()->setSelection(trackId, clipId);
    QApplication::processEvents();

    const auto clipNow = [&]() {
        return window.project().findSequence(sequenceId)->findTrack(trackId)->find(clipId);
    };
    if (!clipNow()->colorCurves.isIdentity()) {
        zaro::app::testing::failf("the clip starts with a hue curve on it\n");
    }

    const int hueIndex = chooser->findData(static_cast<int>(app::CurveEditor::Channel::HueVsSat));
    if (hueIndex < 0) {
        zaro::app::testing::failf("the curve editor does not offer a hue curve\n");
    }
    chooser->setCurrentIndex(hueIndex);
    QApplication::processEvents();
    if (editor->channel() != app::CurveEditor::Channel::HueVsSat) {
        zaro::app::testing::failf("picking the hue channel did not change the editor\n");
    }

    // A quarter of the way round the circle, pulled down from the neutral line
    // half way up. The first press on an empty hue curve seeds the anchors: a
    // curve with one point is still the identity, so without them the first
    // drag would do nothing and look broken.
    const QRect plot = editor->plotArea();
    const QPointF at(plot.left() + (plot.width() / 4), plot.bottom() - (plot.height() / 2));
    const QPointF pulled(at.x(), at.y() + (plot.height() * 0.3));
    {
        QMouseEvent press(QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(editor, &press);
        QMouseEvent move(QEvent::MouseMove, pulled, pulled, Qt::NoButton, Qt::LeftButton,
                         Qt::NoModifier);
        QCoreApplication::sendEvent(editor, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, pulled, pulled, Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(editor, &release);
    }
    QApplication::processEvents();

    const zaro::model::ColorCurves& drawn = clipNow()->colorCurves;
    if (drawn.isIdentity()) {
        zaro::app::testing::failf("dragging in the hue curve did not reach the clip\n");
    }
    // Pulled below neutral where it was dragged, and still neutral on the far
    // side of the circle: an adjustment at one hue is not an adjustment at all
    // of them.
    if (drawn.againstHue.valueAt(0.25) >= 0.45) {
        zaro::app::testing::failf("the point did not come down (%.3f)\n",
                                  drawn.againstHue.valueAt(0.25));
    }
    if (std::fabs(drawn.againstHue.valueAt(0.75) - 0.5) > 0.1) {
        zaro::app::testing::failf("the far side of the circle moved too (%.3f)\n",
                                  drawn.againstHue.valueAt(0.75));
    }
    // The tone curves are untouched: they are a separate gesture and a separate
    // undo step.
    if (!clipNow()->curves.isIdentity()) {
        zaro::app::testing::failf("drawing a hue curve wrote a tone curve\n");
    }

    // And the luma channel, which is the same control on a different axis: no
    // wrap, and a real far end rather than a circle.
    const int lumaIndex = chooser->findData(static_cast<int>(app::CurveEditor::Channel::LumaVsSat));
    if (lumaIndex < 0) {
        zaro::app::testing::failf("the curve editor does not offer a luma curve\n");
    }
    chooser->setCurrentIndex(lumaIndex);
    QApplication::processEvents();
    const QRect lumaPlot = editor->plotArea();
    const QPointF dark(lumaPlot.left() + 1, lumaPlot.bottom() - (lumaPlot.height() / 2));
    const QPointF stripped(dark.x(), dark.y() + (lumaPlot.height() * 0.4));
    {
        QMouseEvent press(QEvent::MouseButtonPress, dark, dark, Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(editor, &press);
        QMouseEvent move(QEvent::MouseMove, stripped, stripped, Qt::NoButton, Qt::LeftButton,
                         Qt::NoModifier);
        QCoreApplication::sendEvent(editor, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, stripped, stripped, Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(editor, &release);
    }
    QApplication::processEvents();
    if (clipNow()->colorCurves.againstLuma.isIdentity()) {
        zaro::app::testing::failf("dragging in the luma curve did not reach the clip\n");
    }
    if (clipNow()->colorCurves.againstLuma.valueAt(0.0) >= 0.45) {
        zaro::app::testing::failf("the shadows were not stripped (%.3f)\n",
                                  clipNow()->colorCurves.againstLuma.valueAt(0.0));
    }
    // The hue curve drawn a moment ago is still there: two channels of one
    // control surface, not one that overwrites the other.
    if (clipNow()->colorCurves.againstHue.isIdentity()) {
        zaro::app::testing::failf("drawing the luma curve threw the hue curve away\n");
    }

    // And the hue shift, the one channel that moves a colour rather than
    // scaling it.
    const int shiftIndex = chooser->findData(static_cast<int>(app::CurveEditor::Channel::HueVsHue));
    if (shiftIndex < 0) {
        zaro::app::testing::failf("the curve editor does not offer a hue shift\n");
    }
    chooser->setCurrentIndex(shiftIndex);
    QApplication::processEvents();
    const QRect shiftPlot = editor->plotArea();
    const QPointF neutral(shiftPlot.left() + (shiftPlot.width() / 2),
                          shiftPlot.bottom() - (shiftPlot.height() / 2));
    const QPointF pushed(neutral.x(), neutral.y() - (shiftPlot.height() * 0.3));
    {
        QMouseEvent press(QEvent::MouseButtonPress, neutral, neutral, Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(editor, &press);
        QMouseEvent move(QEvent::MouseMove, pushed, pushed, Qt::NoButton, Qt::LeftButton,
                         Qt::NoModifier);
        QCoreApplication::sendEvent(editor, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, pushed, pushed, Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(editor, &release);
    }
    QApplication::processEvents();
    if (clipNow()->colorCurves.hueShift.isIdentity()) {
        zaro::app::testing::failf("dragging in the hue shift did not reach the clip\n");
    }
    if (clipNow()->colorCurves.hueShift.valueAt(0.5) <= 0.55) {
        zaro::app::testing::failf("the hue was not pushed (%.3f)\n",
                                  clipNow()->colorCurves.hueShift.valueAt(0.5));
    }
    // The two drawn before it are still there: six channels of one control
    // surface, not one that overwrites the last.
    if (clipNow()->colorCurves.againstHue.isIdentity() ||
        clipNow()->colorCurves.againstLuma.isIdentity()) {
        zaro::app::testing::failf("drawing the hue shift threw an earlier curve away\n");
    }

    std::printf("  colour curves: a quarter turn down, shadows stripped, a hue pushed\n");

    chooser->setCurrentIndex(0);
    QApplication::processEvents();
    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
}
