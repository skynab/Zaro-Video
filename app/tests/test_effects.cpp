// Effects and keyframes: what the parameter panel writes, and what comes out.
//
// Driven through the real window against the real compositor. See GuiFixture.h
// for what is shared and why.

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QListWidget>
#include <QMouseEvent>
#include <QToolButton>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

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
