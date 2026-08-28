// Masks, tracking, stabilisation and keying.
//
// Driven through the real window against the real compositor. See GuiFixture.h
// for what is shared and why.

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QMouseEvent>

#include <catch2/catch_test_macros.hpp>

#include "../FrameGrab.h"
#include "GuiFixture.h"

// The suite was written inside main(), which had this at file scope; the
// bodies still say `model::` and `Status` unqualified.
using namespace zaro;

using zaro::app::dragOnTimeline;
using zaro::app::settledGrab;

// A bezier mask, through the real preview.
//
// A path is the one mask shape the GPU compositor cannot answer from
// uniforms, so this is also a check that the fallback to the CPU graph
// is wired: if it were not, a clip with a path mask would show
// unmasked, which looks like the mask having no effect rather than like
// a path that never reached the shader.
TEST_CASE("A bezier mask, through the real preview", "[gui]") {
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

    const auto pathSequenceId = window.project().activeSequence();
    const auto& pathTracks = window.project().findSequence(pathSequenceId)->videoTracks();
    const auto pathTrackId = pathTracks.size() > 1 ? pathTracks[1].id() : pathTracks.front().id();
    const auto pathRate = window.project().findSequence(pathSequenceId)->frameRate();

    zaro::model::Graphic panel;
    panel.kind = zaro::model::GraphicKind::Rectangle;
    panel.width = 4000.0;
    panel.height = 4000.0;
    panel.red = 1.0;
    panel.green = 1.0;
    panel.blue = 1.0;
    auto added =
        zaro::edit::makeAddGraphic(window.project(), {pathSequenceId, pathTrackId}, panel,
                                   zaro::time::TimeRange{zaro::time::RationalTime{0, pathRate},
                                                         zaro::time::RationalTime{40, pathRate}});
    if (!added) {
        zaro::app::testing::failf("%s\n", added.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*added));
    const auto panelId =
        window.project().findSequence(pathSequenceId)->findTrack(pathTrackId)->clips().front().id;

    window.setPosition(zaro::time::RationalTime{10, pathRate});
    window.renderCache().clear();
    const double whole = meanGray(settledGrab(window.monitor()));

    // A triangle: a shape neither a rectangle nor an ellipse can make.
    zaro::model::Mask triangle;
    triangle.shape = zaro::model::MaskShape::Path;
    const auto* shaped = window.project().findSequence(pathSequenceId);
    const double halfW = shaped->width() / 2.0;
    const double halfH = shaped->height() / 2.0;
    zaro::model::MaskPoint a;
    a.x = -halfW;
    a.y = -halfH;
    zaro::model::MaskPoint b;
    b.x = halfW;
    b.y = -halfH;
    zaro::model::MaskPoint c;
    c.x = -halfW;
    c.y = halfH;
    triangle.path.points = {a, b, c};
    auto masked =
        zaro::edit::makeSetMask(window.project(), {pathSequenceId, pathTrackId}, panelId, triangle);
    if (!masked) {
        zaro::app::testing::failf("%s\n", masked.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*masked));
    window.renderCache().clear();
    const double halved = meanGray(settledGrab(window.monitor()));

    std::printf("  bezier mask: %.1f whole, %.1f through a triangle\n", whole, halved);
    // A triangle covering half the frame leaves about half the light.
    if (!(halved < whole * 0.75) || !(halved > whole * 0.25)) {
        zaro::app::testing::failf("the path mask did not reach the picture as a triangle\n");
    }

    // And the editor: convert the shape to a path through the panel,
    // then drag a point with the mouse and watch the picture follow.
    timeline->selectOnly(pathTrackId, panelId);
    window.effects()->setSelection(pathTrackId, panelId);
    QApplication::processEvents();

    // Back to a rectangle first, so there is a shape to convert.
    zaro::model::Mask box;
    box.shape = zaro::model::MaskShape::Rectangle;
    box.width = shaped->width() / 2.0;
    box.height = shaped->height() / 2.0;
    auto boxed =
        zaro::edit::makeSetMask(window.project(), {pathSequenceId, pathTrackId}, panelId, box);
    if (!boxed) {
        zaro::app::testing::failf("%s\n", boxed.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*boxed));
    window.effects()->setSelection(pathTrackId, panelId);
    QApplication::processEvents();

    auto* convert = window.effects()->findChild<QPushButton*>("mask-to-path");
    if (convert == nullptr) {
        zaro::app::testing::failf("there is no convert-to-path button\n");
    }
    window.renderCache().clear();
    const double asRectangle = meanGray(settledGrab(window.monitor()));
    convert->click();
    QApplication::processEvents();

    const auto* converted =
        window.project().findSequence(pathSequenceId)->findTrack(pathTrackId)->find(panelId);
    if (converted->mask.shape != zaro::model::MaskShape::Path ||
        converted->mask.path.points.size() != 4) {
        zaro::app::testing::failf("converting did not produce a four-point path\n");
    }
    window.renderCache().clear();
    const double asPath = meanGray(settledGrab(window.monitor()));
    if (std::fabs(asPath - asRectangle) > 3.0) {
        zaro::app::testing::failf(
            "converting a shape to a path changed the picture "
            "(%.1f -> %.1f)\n",
            asRectangle, asPath);
    }

    // Drag the top-left point out to the corner of the frame, through
    // the overlay, with real mouse events.
    auto* overlay = window.monitor()->findChild<zaro::app::MaskOverlay*>();
    if (overlay == nullptr || !overlay->isEditing()) {
        zaro::app::testing::failf("the mask overlay is not editing the path\n");
    }
    const QRectF picture = window.monitor()->pictureRect();
    const double scale = picture.width() / shaped->width();
    // A copy, not a reference: the drag mutates the model underneath,
    // and a reference would compare the moved point against itself.
    const zaro::model::MaskPoint corner = converted->mask.path.points.front();
    const QPointF grabAt{picture.center().x() + (corner.x * scale),
                         picture.center().y() + (corner.y * scale)};
    const QPointF dropAt{picture.left() + 2.0, picture.top() + 2.0};

    QMouseEvent press(QEvent::MouseButtonPress, grabAt, grabAt, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(overlay, &press);
    QMouseEvent move(QEvent::MouseMove, dropAt, dropAt, Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QCoreApplication::sendEvent(overlay, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, dropAt, dropAt, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(overlay, &release);
    QApplication::processEvents();

    const auto* dragged =
        window.project().findSequence(pathSequenceId)->findTrack(pathTrackId)->find(panelId);
    const auto& movedCorner = dragged->mask.path.points.front();
    window.renderCache().clear();
    const double afterDrag = meanGray(settledGrab(window.monitor()));

    std::printf(
        "  mask editor: corner %.0f,%.0f -> %.0f,%.0f; %.1f masked, %.1f after "
        "dragging\n",
        corner.x, corner.y, movedCorner.x, movedCorner.y, asPath, afterDrag);
    if (std::fabs(movedCorner.x - corner.x) < 10.0) {
        zaro::app::testing::failf("dragging a point did not move it\n");
    }
    if (!(afterDrag > asPath + 5.0)) {
        zaro::app::testing::failf(
            "enlarging the mask did not let more of the picture "
            "through\n");
    }
    // One drag, one undo step.
    window.commands().undo(window.project());
    const auto* undone =
        window.project().findSequence(pathSequenceId)->findTrack(pathTrackId)->find(panelId);
    if (std::fabs(undone->mask.path.points.front().x - corner.x) > 0.5) {
        zaro::app::testing::failf("one drag was not one undo step\n");
    }

    // The pen: draw a fresh path with clicks on the picture, and
    // close it by clicking the first point again.
    auto* pen = window.effects()->findChild<QPushButton*>("mask-draw");
    if (pen == nullptr) {
        zaro::app::testing::failf("there is no draw-path button\n");
    }
    auto clickOverlay = [&](const QPointF& at) {
        QMouseEvent down(QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton,
                         Qt::NoModifier);
        QCoreApplication::sendEvent(overlay, &down);
        QMouseEvent up(QEvent::MouseButtonRelease, at, at, Qt::LeftButton, Qt::NoButton,
                       Qt::NoModifier);
        QCoreApplication::sendEvent(overlay, &up);
    };
    // A triangle across most of the frame, so it lets through
    // noticeably more than the quarter-frame rectangle did.
    const QPointF penPoints[] = {{picture.center().x(), picture.top() + 4.0},
                                 {picture.right() - 4.0, picture.bottom() - 4.0},
                                 {picture.left() + 4.0, picture.bottom() - 4.0}};

    pen->setChecked(true);
    QApplication::processEvents();
    if (!overlay->isDrawing()) {
        zaro::app::testing::failf("the pen button did not start the pen\n");
    }
    for (const QPointF& at : penPoints) {
        clickOverlay(at);
    }
    // Still nothing written: the path is not a mask until it closes.
    const auto* midDraw =
        window.project().findSequence(pathSequenceId)->findTrack(pathTrackId)->find(panelId);
    if (midDraw->mask.path.points.size() != 4) {
        zaro::app::testing::failf("laying down points changed the clip's mask\n");
    }
    clickOverlay(penPoints[0]);
    QApplication::processEvents();

    const auto* drawn =
        window.project().findSequence(pathSequenceId)->findTrack(pathTrackId)->find(panelId);
    if (drawn->mask.shape != zaro::model::MaskShape::Path || drawn->mask.path.points.size() != 3) {
        zaro::app::testing::failf("the pen did not close a three-point path\n");
    }
    if (overlay->isDrawing() || pen->isChecked()) {
        zaro::app::testing::failf("closing the path left the pen out\n");
    }
    window.renderCache().clear();
    const double penned = meanGray(settledGrab(window.monitor()));
    std::printf("  pen tool: %zu points drawn, %.1f masked before, %.1f after\n",
                drawn->mask.path.points.size(), afterDrag, penned);
    if (!(penned > afterDrag + 5.0)) {
        zaro::app::testing::failf("the drawn path did not replace the old mask\n");
    }
    // One drawing, one undo step.
    window.commands().undo(window.project());
    const auto* unpenned =
        window.project().findSequence(pathSequenceId)->findTrack(pathTrackId)->find(panelId);
    if (unpenned->mask.path.points.size() != 4) {
        zaro::app::testing::failf("drawing a path was not one undo step\n");
    }

    // Escape abandons without writing anything.
    pen->setChecked(true);
    clickOverlay(penPoints[0]);
    clickOverlay(penPoints[1]);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(overlay, &escape);
    QApplication::processEvents();
    if (overlay->isDrawing() || !window.commands().canRedo()) {
        zaro::app::testing::failf("escape did not abandon the drawing cleanly\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.renderCache().clear();
    window.monitor()->update();
    QApplication::processEvents();
}

// Mask tracking, against motion with a known answer.
//
// A title moving a fixed number of pixels a frame, so the tracker's
// answer can be checked rather than merely looked at: real footage
// would only tell us the mask went roughly the right way.
TEST_CASE("Mask tracking follows motion with a known answer", "[gui]") {
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

    const auto trackSequenceId = window.project().activeSequence();
    const auto* trackSequence = window.project().findSequence(trackSequenceId);
    const auto trackTrackId = trackSequence->videoTracks().back().id();
    const auto trackRate = trackSequence->frameRate();
    constexpr int kTrackedFrames = 8;
    constexpr double kPerFrameX = 6.0;
    constexpr double kPerFrameY = -4.0;

    zaro::model::Graphic mark;
    mark.kind = zaro::model::GraphicKind::Text;
    mark.text = "TRACK ME";
    mark.pointSize = 60.0;
    mark.bold = true;
    mark.width = 300.0;
    mark.height = 120.0;
    mark.red = 1.0;
    mark.green = 1.0;
    mark.blue = 1.0;
    auto placed = zaro::edit::makeAddGraphic(
        window.project(), {trackSequenceId, trackTrackId}, mark,
        zaro::time::TimeRange{zaro::time::RationalTime{0, trackRate},
                              zaro::time::RationalTime{kTrackedFrames + 2, trackRate}});
    if (!placed) {
        zaro::app::testing::failf("%s\n", placed.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*placed));
    // Found by what it is, not by where it landed: the track already
    // has clips on it, so "the last one" is not necessarily ours.
    zaro::model::ClipId markId;
    zaro::time::RationalTime markStart;
    for (const auto& candidate :
         window.project().findSequence(trackSequenceId)->findTrack(trackTrackId)->clips()) {
        if (candidate.graphic.text == "TRACK ME") {
            markId = candidate.id;
            markStart = candidate.start();
        }
    }
    if (!markId.isValid()) {
        zaro::app::testing::failf("the title to track was not added\n");
    }

    // Two linear keyframes across the tracked span, so the motion per
    // frame is exactly what the check expects.
    for (const auto& [param, perFrame] : {std::pair{zaro::model::Param::PositionX, kPerFrameX},
                                          std::pair{zaro::model::Param::PositionY, kPerFrameY}}) {
        zaro::model::Curve moving;
        moving.set(zaro::model::Keyframe{zaro::time::RationalTime{0, trackRate},
                                         0.0,
                                         zaro::model::Interpolation::Linear,
                                         {},
                                         {}});
        moving.set(zaro::model::Keyframe{zaro::time::RationalTime{kTrackedFrames, trackRate},
                                         perFrame * kTrackedFrames,
                                         zaro::model::Interpolation::Linear,
                                         {},
                                         {}});
        auto animated = zaro::edit::makeSetCurve(window.project(), {trackSequenceId, trackTrackId},
                                                 markId, param, moving);
        if (!animated) {
            zaro::app::testing::failf("%s\n", animated.error().toString().c_str());
        }
        window.commands().execute(window.project(), std::move(*animated));
    }

    // A box over the letters where they start.
    zaro::model::Mask over;
    over.shape = zaro::model::MaskShape::Rectangle;
    over.width = 150.0;
    over.height = 90.0;
    auto boxed =
        zaro::edit::makeSetMask(window.project(), {trackSequenceId, trackTrackId}, markId, over);
    if (!boxed) {
        zaro::app::testing::failf("%s\n", boxed.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*boxed));

    timeline->selectOnly(trackTrackId, markId);
    window.effects()->setSelection(trackTrackId, markId);
    window.setPosition(markStart);
    QApplication::processEvents();

    auto* trackButton = window.effects()->findChild<QPushButton*>("mask-track");
    if (trackButton == nullptr || !trackButton->isEnabled()) {
        zaro::app::testing::failf("there is no usable track-mask button\n");
    }

    auto tracked = window.trackMaskForward();
    if (!tracked) {
        zaro::app::testing::failf("%s\n", tracked.error().toString().c_str());
    }
    const auto* followed =
        window.project().findSequence(trackSequenceId)->findTrack(trackTrackId)->find(markId);
    const auto end = markStart + zaro::time::RationalTime{kTrackedFrames, trackRate};
    const double gotX = followed->parameterAt(zaro::model::Param::MaskX, end);
    const double gotY = followed->parameterAt(zaro::model::Param::MaskY, end);
    const double wantX = kPerFrameX * kTrackedFrames;
    const double wantY = kPerFrameY * kTrackedFrames;
    std::printf(
        "  mask track: %d frames, weakest %.2f, ended at %.1f,%.1f (wanted "
        "%.1f,%.1f)\n",
        tracked->frames, tracked->confidence, gotX, gotY, wantX, wantY);
    if (tracked->frames < kTrackedFrames) {
        zaro::app::testing::failf("the track stopped after %d frames: %s\n", tracked->frames,
                                  tracked->stopped.c_str());
    }
    // Within a pixel and a half after eight frames of accumulating:
    // frame-to-frame tracking drifts, and the test says how much drift
    // is still a working tracker.
    if (std::fabs(gotX - wantX) > 1.5 || std::fabs(gotY - wantY) > 1.5) {
        zaro::app::testing::failf("the mask did not follow the picture\n");
    }
    // And the mask really moved with it: masked to the letters, the
    // frame at the end would go dark if the mask had stayed put.
    window.setPosition(end);
    window.renderCache().clear();
    const double lit = meanGray(settledGrab(window.monitor()));
    window.commands().undo(window.project());  // the track, in one step
    const auto* untracked =
        window.project().findSequence(trackSequenceId)->findTrack(trackTrackId)->find(markId);
    if (untracked->animation.find(zaro::model::Param::MaskX) != nullptr ||
        untracked->animation.find(zaro::model::Param::MaskY) != nullptr) {
        zaro::app::testing::failf("undoing the track left keyframes behind\n");
    }
    window.renderCache().clear();
    const double stranded = meanGray(settledGrab(window.monitor()));
    // Five per cent of the whole grab: the mask is a fifth of the
    // frame and the frame is a fraction of the widget, so a mask that
    // moved off what it was on cannot change the average by much. What
    // matters is that it changes it at all and in the right direction.
    if (!(lit > stranded * 1.05)) {
        zaro::app::testing::failf(
            "tracking the mask showed no more of the picture than "
            "leaving it behind did (%.2f vs %.2f)\n",
            lit, stranded);
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.renderCache().clear();
    window.monitor()->update();
    QApplication::processEvents();
}

// Stabilisation, end to end on a clip that really shakes.
//
// Whether the arithmetic holds a synthetic camera path still is settled
// headlessly. What is checked here is the whole chain: decode the
// clip's own frames, analyse them, write curves, and have the picture
// that comes out of the compositor move less than it did.
TEST_CASE("Stabilisation steadies a clip that shakes", "[gui]") {
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

    auto probed =
        zaro::platform::ffmpeg::probe(zaro::app::testing::mediaFixture("shaky_texture.mov"));
    if (!probed) {
        zaro::app::testing::failf("%s (run testdata/generate.sh)\n",
                                  probed.error().toString().c_str());
    }
    zaro::model::MediaRef shaky;
    shaky.path = probed->path;
    shaky.name = "shaky";
    shaky.info = *probed;
    auto imported = zaro::edit::makeImportMedia(window.project(), shaky);
    if (!imported) {
        zaro::app::testing::failf("%s\n", imported.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*imported));
    const auto shakyId = window.project().media().back().id;
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }

    const auto stabSequenceId = window.project().activeSequence();
    const auto* stabSequence = window.project().findSequence(stabSequenceId);
    const auto stabRate = stabSequence->frameRate();
    const auto stabTrackId = stabSequence->videoTracks().back().id();
    constexpr int kStabFrames = 20;
    const auto mediaRate = probed->primaryVideo()->frameRate;
    auto placed = zaro::edit::makePlaceFromSource(
        window.project(), {stabSequenceId, stabTrackId}, shakyId,
        zaro::time::TimeRange{zaro::time::RationalTime{0, mediaRate},
                              zaro::time::RationalTime{kStabFrames, mediaRate}},
        zaro::time::RationalTime{0, stabRate}, zaro::edit::PlaceMode::Overwrite);
    if (!placed) {
        zaro::app::testing::failf("%s\n", placed.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*placed));
    zaro::model::ClipId stabClipId;
    for (const auto& candidate :
         window.project().findSequence(stabSequenceId)->findTrack(stabTrackId)->clips()) {
        if (candidate.source == shakyId) {
            stabClipId = candidate.id;
        }
    }
    if (!stabClipId.isValid()) {
        zaro::app::testing::failf("the shaky clip was not placed\n");
    }
    const auto stabStart = window.project()
                               .findSequence(stabSequenceId)
                               ->findTrack(stabTrackId)
                               ->find(stabClipId)
                               ->start();
    const auto stabAt = stabStart + zaro::time::RationalTime{2, stabRate};

    // How much the composited picture moves frame to frame, measured
    // the same way both times.
    auto shakiness = [&]() {
        zaro::render::RenderGraph graph{window.frameSource()};
        graph.setProject(&window.project());
        zaro::render::RgbaImage previous;
        double worst = 0.0;
        const auto* live = window.project().findSequence(stabSequenceId);
        for (int f = 0; f < kStabFrames; ++f) {
            zaro::render::RgbaImage current;
            const auto when = stabStart + zaro::time::RationalTime{f, stabRate};
            if (Status ok = graph.compositeInto(*live, when, current); !ok) {
                return -1.0;
            }
            if (previous.isValid()) {
                zaro::render::PatchWindow centre;
                centre.centreX = static_cast<double>(current.width()) / 2.0;
                centre.centreY = static_cast<double>(current.height()) / 2.0;
                centre.halfWidth = static_cast<double>(current.width()) / 4.0;
                centre.halfHeight = static_cast<double>(current.height()) / 4.0;
                centre.search = 24.0;
                const auto moved = zaro::render::trackPatch(previous, current, centre);
                if (moved.usable) {
                    worst = std::max(worst, std::hypot(moved.dx, moved.dy));
                }
            }
            previous = std::move(current);
        }
        return worst;
    };

    const double shookBefore = shakiness();

    timeline->selectOnly(stabTrackId, stabClipId);
    window.effects()->setSelection(stabTrackId, stabClipId);
    window.setPosition(stabAt);
    QApplication::processEvents();

    auto* stabButton = window.effects()->findChild<QPushButton*>("stabilise");
    auto* clearButton = window.effects()->findChild<QPushButton*>("stabilise-clear");
    if (stabButton == nullptr || clearButton == nullptr || !stabButton->isEnabled()) {
        zaro::app::testing::failf("there is no usable stabilise button\n");
    }
    if (clearButton->isEnabled()) {
        zaro::app::testing::failf("an unstabilised clip offers to clear nothing\n");
    }

    const double scaleBefore = window.project()
                                   .findSequence(stabSequenceId)
                                   ->findTrack(stabTrackId)
                                   ->find(stabClipId)
                                   ->transformAt(stabAt)
                                   .scaleX;
    auto held = window.stabiliseClip();
    if (!held) {
        zaro::app::testing::failf("%s\n", held.error().toString().c_str());
    }
    const auto* steadied =
        window.project().findSequence(stabSequenceId)->findTrack(stabTrackId)->find(stabClipId);
    if (steadied->animation.find(zaro::model::Param::StabiliseX) == nullptr ||
        steadied->animation.find(zaro::model::Param::StabiliseZoom) == nullptr) {
        zaro::app::testing::failf("stabilising wrote no curves\n");
    }
    const double scaleAfter = steadied->transformAt(stabAt).scaleX;
    if (!(scaleAfter > scaleBefore)) {
        zaro::app::testing::failf(
            "the stabilise zoom never reached the transform "
            "(%.4f -> %.4f)\n",
            scaleBefore, scaleAfter);
    }

    window.renderCache().clear();
    const double shookAfter = shakiness();
    std::printf("  stabilise: %d frames, zoom %.3f, worst move %.1f px -> %.1f px\n",
                held->measured, held->zoom, shookBefore, shookAfter);
    if (shookBefore < 4.0) {
        zaro::app::testing::failf("the shaky fixture does not shake (%.1f px)\n", shookBefore);
    }
    if (!(shookAfter < shookBefore / 2.0)) {
        zaro::app::testing::failf("stabilising did not steady the picture\n");
    }

    window.effects()->setSelection(stabTrackId, stabClipId);
    QApplication::processEvents();
    if (!clearButton->isEnabled()) {
        zaro::app::testing::failf("a stabilised clip offers no way to clear it\n");
    }
    // Clearing is exactly what it says: the framing, back.
    clearButton->click();
    QApplication::processEvents();
    const auto* cleared =
        window.project().findSequence(stabSequenceId)->findTrack(stabTrackId)->find(stabClipId);
    if (cleared->animation.find(zaro::model::Param::StabiliseX) != nullptr ||
        cleared->transformAt(stabAt).scaleX != scaleBefore) {
        zaro::app::testing::failf("clearing the stabilisation left it in\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    // Importing is a command too, so undo has already taken the file
    // back out; the decoder has to be told.
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    window.renderCache().clear();
    window.monitor()->update();
    QApplication::processEvents();
}

// Keying, through the real GPU compositor and the real panel.
//
// A green rectangle stands in for a green screen: what is being checked
// is that a key reaches the picture and takes alpha with it, not that
// the maths is right -- that is checked headlessly and against the CPU
// reference frame by frame.
TEST_CASE("Chroma keying, through the real compositor", "[gui]") {
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

    const auto keySequenceId = window.project().activeSequence();
    const auto& keyTracks = window.project().findSequence(keySequenceId)->videoTracks();
    const auto keyTop = keyTracks.size() > 1 ? keyTracks[1].id() : keyTracks.front().id();

    std::int64_t keyFrame = 0;
    double beneath = 1e9;
    for (std::int64_t frame = 0; frame < 35; ++frame) {
        window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
        const double gray = meanGray(settledGrab(window.monitor()));
        if (gray < beneath) {
            beneath = gray;
            keyFrame = frame;
        }
    }
    window.setPosition(zaro::time::RationalTime{keyFrame, sequence.frameRate()});
    QApplication::processEvents();

    zaro::model::Graphic screen;
    screen.kind = zaro::model::GraphicKind::Rectangle;
    screen.width = 4000.0;  // larger than the frame, so it fills it
    screen.height = 4000.0;
    screen.red = 0.0;
    screen.green = 1.0;
    screen.blue = 0.0;
    auto placed = zaro::edit::makeAddGraphic(
        window.project(), {keySequenceId, keyTop}, screen,
        zaro::time::TimeRange{zaro::time::RationalTime{0, sequence.frameRate()},
                              zaro::time::RationalTime{40, sequence.frameRate()}});
    if (!placed) {
        zaro::app::testing::failf("%s\n", placed.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*placed));
    const auto screenClipId =
        window.project().findSequence(keySequenceId)->findTrack(keyTop)->clips().front().id;
    window.monitor()->update();
    QApplication::processEvents();
    const double covered = meanGray(settledGrab(window.monitor()));
    if (!(covered > beneath + 50.0)) {
        zaro::app::testing::failf("the green screen did not reach the preview\n");
    }

    // Set the key through the panel, the way somebody would.
    timeline->selectOnly(keyTop, screenClipId);
    window.effects()->setSelection(keyTop, screenClipId);
    QApplication::processEvents();

    auto* kind = window.effects()->findChild<QComboBox*>("key-kind");
    auto* spill = window.effects()->findChild<QDoubleSpinBox*>("key-spill");
    if (kind == nullptr || spill == nullptr) {
        zaro::app::testing::failf("the key controls are not in the panel\n");
    }
    kind->setCurrentIndex(kind->findData(static_cast<int>(zaro::model::KeyKind::Chroma)));
    QApplication::processEvents();
    window.monitor()->update();
    QApplication::processEvents();
    const double keyed = meanGray(settledGrab(window.monitor()));

    std::printf("  chroma key: %.1f behind the screen, %.1f with it, %.1f keyed\n", beneath,
                covered, keyed);
    if (!(keyed < beneath + 10.0)) {
        zaro::app::testing::failf("the key did not reach the picture\n");
    }

    // And switching it off brings the screen back, so what was measured
    // was the key rather than the clip disappearing for some other
    // reason.
    kind->setCurrentIndex(kind->findData(static_cast<int>(zaro::model::KeyKind::None)));
    QApplication::processEvents();
    window.monitor()->update();
    QApplication::processEvents();
    if (!(meanGray(settledGrab(window.monitor())) > beneath + 50.0)) {
        zaro::app::testing::failf("clearing the key did not bring the screen back\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.monitor()->update();
    QApplication::processEvents();
}

// A mask, through the panel and the real GPU compositor. The geometry
// is compared against the CPU headlessly; what that cannot show is
// whether these controls reach the picture.
TEST_CASE("A mask, through the panel and the compositor", "[gui]") {
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

    auto* shapeBox = window.effects()->findChild<QComboBox*>("mask-shape");
    auto* inverted = window.effects()->findChild<QCheckBox*>("mask-inverted");
    if (shapeBox == nullptr || inverted == nullptr) {
        zaro::app::testing::failf("the mask controls are missing\n");
    }
    window.effects()->setSelection(videoTrack.id(), original.id);
    QApplication::processEvents();

    // A lit frame, so a mask that hides most of it is unmistakable.
    std::int64_t litFrame = 0;
    double litness = 0.0;
    for (std::int64_t frame = 0; frame < 40; ++frame) {
        window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
        const double gray = meanGray(settledGrab(window.monitor()));
        if (gray > litness) {
            litness = gray;
            litFrame = frame;
        }
    }
    window.setPosition(zaro::time::RationalTime{litFrame, sequence.frameRate()});
    QApplication::processEvents();
    const double whole = meanGray(settledGrab(window.monitor()));

    // A small ellipse: most of the picture goes.
    zaro::model::Mask mask;
    mask.shape = zaro::model::MaskShape::Ellipse;
    mask.width = 60.0;
    mask.height = 60.0;
    auto built = zaro::edit::makeSetMask(window.project(), {sequence.id(), videoTrack.id()},
                                         original.id, mask);
    if (!built) {
        zaro::app::testing::failf("%s\n", built.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*built));
    window.monitor()->update();
    QApplication::processEvents();
    const double throughMask = meanGray(settledGrab(window.monitor()));

    // Inverted, the same mask keeps everything it was hiding.
    mask.inverted = true;
    auto flipped = zaro::edit::makeSetMask(window.project(), {sequence.id(), videoTrack.id()},
                                           original.id, mask);
    window.commands().execute(window.project(), std::move(*flipped));
    window.monitor()->update();
    QApplication::processEvents();
    const double outside = meanGray(settledGrab(window.monitor()));

    std::printf("  mask: %.1f whole, %.1f through a small ellipse, %.1f inverted\n", whole,
                throughMask, outside);
    if (!(throughMask < whole * 0.5)) {
        zaro::app::testing::failf("the mask did not hide anything\n");
    }
    if (!(outside > throughMask * 2.0)) {
        zaro::app::testing::failf("inverting the mask did not swap what it keeps\n");
    }
    // The two halves have to add up to the whole, since one keeps
    // exactly what the other discards.
    if (std::fabs((throughMask + outside) - whole) > whole * 0.05) {
        zaro::app::testing::failf(
            "a mask and its inverse do not add up to the picture "
            "(%.1f + %.1f against %.1f)\n",
            throughMask, outside, whole);
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.effects()->refresh();
    window.monitor()->update();
    QApplication::processEvents();
}
