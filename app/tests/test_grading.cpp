// Grading: the wheels, the curves, the scopes, and the looks.
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

// The scopes, end to end: a measurement has to reach the panel and be
// drawn there, and it has to be drawn the right way up. Where the trace
// sits is the assertion, not how many pixels it covers -- the
// measurement is in signal order, where 0 is black, and the screen is
// upside down relative to it. Getting that backwards produces a scope
// that looks entirely plausible and reports the opposite of the truth.
TEST_CASE("The scopes measure what the monitor is showing", "[gui]") {
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

    // Scopes are up in the Color workspace. Grabbing a panel that is
    // not on screen returns something, but not what a colourist would
    // be looking at, so the test puts it on screen first.
    window.setWorkspace("Color");
    QApplication::processEvents();
    // Returns the mean row of the trace, as a fraction of the plot
    // area: 0 is the top of the scope and 1 the bottom.
    const auto traceHeight = [&]() -> double {
        const QImage shot = window.scopes()->grab().toImage();
        const auto dpr = static_cast<int>(shot.devicePixelRatio());
        const QRect plot = window.scopes()->plotArea();
        const int top = plot.top() * dpr;
        const int bottom = std::min((plot.bottom() + 1) * dpr, shot.height());
        double weighted = 0.0;
        std::int64_t lit = 0;
        for (int scanY = top; scanY < bottom; ++scanY) {
            for (int x = plot.left() * dpr; x < std::min((plot.right() + 1) * dpr, shot.width());
                 ++x) {
                if (qGray(shot.pixel(x, scanY)) > 110) {
                    weighted += scanY - top;
                    ++lit;
                }
            }
        }
        return lit == 0 ? -1.0 : weighted / static_cast<double>(lit) / std::max(1, bottom - top);
    };

    // This fixture is black except on its flash frames, so it offers a
    // bright frame and a dark one without any setup.
    double brightest = 0.0;
    double darkest = 0.0;
    double atBrightest = -1.0;
    double atDarkest = -1.0;
    for (std::int64_t frame = 0; frame < 40; ++frame) {
        window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
        QApplication::processEvents();
        const double picture = meanGray(settledGrab(window.monitor()));
        const double where = traceHeight();
        if (where < 0.0) {
            zaro::app::testing::failf("the scope drew no trace at all\n");
        }
        if (atBrightest < 0.0 || picture > brightest) {
            brightest = picture;
            atBrightest = where;
        }
        if (atDarkest < 0.0 || picture < darkest) {
            darkest = picture;
            atDarkest = where;
        }
    }
    std::printf(
        "  scope trace sits at %.2f of the plot on the brightest frame, %.2f on "
        "the darkest\n",
        atBrightest, atDarkest);
    if (!(atBrightest < atDarkest)) {
        zaro::app::testing::failf(
            "the scope puts a bright picture no higher than a dark one, "
            "so it is drawn upside down or not measuring the picture\n");
    }
    if (atDarkest < 0.8) {
        zaro::app::testing::failf("a black frame should read at the bottom of the scope\n");
    }
    window.setWorkspace("Edit");
    QApplication::processEvents();
}

// Colour correction, through the panel and out to the picture. The
// grade is separate code on the CPU and the GPU, and the unit tests
// compare those two directly -- what they cannot see is whether the
// panel is wired to either of them.
TEST_CASE("Colour correction reaches the picture", "[gui]") {
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

    auto* exposure = window.effects()
                         ->findChild<QToolButton*>("keyframe:exposure")
                         ->parentWidget()
                         ->findChild<QDoubleSpinBox*>();
    auto* saturation = window.effects()
                           ->findChild<QToolButton*>("keyframe:saturation")
                           ->parentWidget()
                           ->findChild<QDoubleSpinBox*>();
    if (exposure == nullptr || saturation == nullptr) {
        zaro::app::testing::failf("the colour controls are missing\n");
    }
    window.effects()->setSelection(videoTrack.id(), original.id);

    // A frame that is lit to begin with: exposure on black is black.
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

    const double litBefore = meanGray(settledGrab(window.monitor()));
    exposure->setValue(-2.0);
    QApplication::processEvents();
    window.monitor()->update();
    QApplication::processEvents();
    const double darker = meanGray(settledGrab(window.monitor()));

    exposure->setValue(0.0);
    saturation->setValue(0.0);
    QApplication::processEvents();
    window.monitor()->update();
    QApplication::processEvents();
    const QImage grey = settledGrab(window.monitor());
    std::int64_t coloured = 0;
    std::int64_t looked = 0;
    for (int gy = 0; gy < grey.height(); gy += 3) {
        for (int gx = 0; gx < grey.width(); gx += 3) {
            const QColor sample = grey.pixelColor(gx, gy);
            ++looked;
            if (std::abs(sample.red() - sample.green()) > 4 ||
                std::abs(sample.green() - sample.blue()) > 4) {
                ++coloured;
            }
        }
    }
    std::printf(
        "  colour: two stops down %.1f -> %.1f, monochrome leaves %lld of %lld "
        "pixels coloured\n",
        litBefore, darker, static_cast<long long>(coloured), static_cast<long long>(looked));

    if (!(darker < litBefore * 0.6)) {
        zaro::app::testing::failf(
            "two stops of exposure did not darken the picture; the "
            "panel and the compositor are not connected\n");
    }
    if (coloured > looked / 100) {
        zaro::app::testing::failf("zero saturation left colour in the picture\n");
    }

    saturation->setValue(100.0);
    QApplication::processEvents();
    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.monitor()->update();
    QApplication::processEvents();
}

// The colour wheels, through the real panel and out to the picture.
//
// Lifting the shadows on a clip that is mostly black is the change this
// fixture can show: an offset adds the same amount everywhere, so black
// stops being black.
TEST_CASE("The colour wheels lift the shadows", "[gui]") {
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

    const auto wheelSequenceId = window.project().activeSequence();
    const auto wheelTrackId =
        window.project().findSequence(wheelSequenceId)->videoTracks().front().id();
    const auto* wheelTrack =
        window.project().findSequence(wheelSequenceId)->findTrack(wheelTrackId);
    if (wheelTrack->clips().empty()) {
        zaro::app::testing::failf("no clip to grade\n");
    }
    const auto wheelClipId = wheelTrack->clips().front().id;
    const auto wheelRate = window.project().findSequence(wheelSequenceId)->frameRate();

    timeline->selectOnly(wheelTrackId, wheelClipId);
    window.effects()->setSelection(wheelTrackId, wheelClipId);
    window.setPosition(zaro::time::RationalTime{6, wheelRate});
    QApplication::processEvents();
    const double beforeLift = meanGray(settledGrab(window.monitor()));

    auto* shadowRed = window.effects()->findChild<QDoubleSpinBox*>("wheel-0-0");
    auto* shadowGreen = window.effects()->findChild<QDoubleSpinBox*>("wheel-0-1");
    auto* shadowBlue = window.effects()->findChild<QDoubleSpinBox*>("wheel-0-2");
    if (shadowRed == nullptr || shadowGreen == nullptr || shadowBlue == nullptr) {
        zaro::app::testing::failf("the wheel controls are not in the panel\n");
    }
    shadowRed->setValue(0.25);
    shadowGreen->setValue(0.25);
    shadowBlue->setValue(0.25);
    QApplication::processEvents();

    const auto* graded =
        window.project().findSequence(wheelSequenceId)->findTrack(wheelTrackId)->find(wheelClipId);
    if (graded == nullptr || graded->wheels.offsetR != 0.25) {
        zaro::app::testing::failf("the wheels did not reach the clip\n");
    }
    const double afterLift = meanGray(settledGrab(window.monitor()));
    std::printf("  colour wheels: %.1f before lifting the shadows, %.1f after\n", beforeLift,
                afterLift);
    if (!(afterLift > beforeLift + 20.0)) {
        zaro::app::testing::failf("the wheels did not reach the picture\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.monitor()->update();
    QApplication::processEvents();
}

// Shot matching, through the real window.
//
// This fixture's lit frames are flat white, and three anchors cannot be
// built from a frame with no range in it. A vignette makes one: the
// same flat frame with its corners pulled down is a gradient, which is
// enough to match on. Using a feature already here beats inventing a
// fixture, and it keeps what is measured to the real pipeline.
TEST_CASE("Shot matching brings two shots together", "[gui]") {
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

    const auto matchSequenceId = window.project().activeSequence();
    const auto matchTrackId =
        window.project().findSequence(matchSequenceId)->videoTracks().front().id();
    const auto matchClipId =
        window.project().findSequence(matchSequenceId)->findTrack(matchTrackId)->clips().front().id;
    const auto matchRate = window.project().findSequence(matchSequenceId)->frameRate();

    zaro::model::Vignette gradient;
    gradient.amount = -0.9;
    gradient.midpoint = 0.1;
    gradient.feather = 1.2;
    auto shaped = zaro::edit::makeSetVignette(window.project(), {matchSequenceId, matchTrackId},
                                              matchClipId, gradient);
    if (!shaped) {
        zaro::app::testing::failf("%s\n", shaped.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*shaped));

    // A second clip of the same material, same gradient, graded away.
    zaro::model::Clip second =
        *window.project().findSequence(matchSequenceId)->findTrack(matchTrackId)->find(matchClipId);
    second.id = window.project().ids().next<zaro::model::ClipTag>();
    second.timelineRange = zaro::time::TimeRange{zaro::time::RationalTime{600, matchRate},
                                                 zaro::time::RationalTime{40, matchRate}};
    second.sourceRange =
        zaro::time::TimeRange{second.sourceRange.start(), zaro::time::RationalTime{40, matchRate}};
    second.color.exposure = -1.0;
    second.color.temperature = 25.0;
    const auto secondId = second.id;
    auto placed =
        zaro::edit::makeOverwrite(window.project(), {matchSequenceId, matchTrackId}, second);
    if (!placed) {
        zaro::app::testing::failf("%s\n", placed.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*placed));
    window.renderCache().clear();

    // Hold a frame of the first, stand on the second, and match.
    timeline->selectOnly(matchTrackId, secondId);
    window.effects()->setSelection(matchTrackId, secondId);
    window.setComparing(true, zaro::time::RationalTime{0, matchRate});
    window.setPosition(zaro::time::RationalTime{600, matchRate});
    QApplication::processEvents();

    auto apart = window.matchToReference();
    if (!apart) {
        zaro::app::testing::failf("%s\n", apart.error().toString().c_str());
    }
    std::printf("  shot match: two shots were %.4f apart, now %.4f (%s)\n", apart->before,
                apart->after, apart->usable ? "applied" : apart->reason.c_str());
    if (!apart->usable) {
        zaro::app::testing::failf(
            "two shots of the same material were called "
            "unmatchable: %s\n",
            apart->reason.c_str());
    }
    if (!(apart->after < apart->before)) {
        zaro::app::testing::failf("the match did not bring them closer\n");
    }
    const auto* corrected =
        window.project().findSequence(matchSequenceId)->findTrack(matchTrackId)->find(secondId);
    if (corrected == nullptr || corrected->wheels.isIdentity()) {
        zaro::app::testing::failf("the match reached nothing\n");
    }

    // And a refusal is a refusal: two frames with no range in them
    // cannot be matched, and nothing is applied on somebody's behalf.
    window.commands().undo(window.project());  // take the wheels back off
    zaro::render::RgbaImage flatDim{16, 16};
    flatDim.fill(zaro::render::Rgba{0.5F, 0.5F, 0.5F, 1.0F});
    zaro::render::RgbaImage flatBright{16, 16};
    flatBright.fill(zaro::render::Rgba{1.0F, 1.0F, 1.0F, 1.0F});
    auto refused = zaro::render::matchShot(flatDim, flatBright);
    if (!refused || refused->usable || refused->reason.empty()) {
        zaro::app::testing::failf("two flat frames were reported as matchable\n");
    }

    window.setComparing(false, zaro::time::RationalTime{0, matchRate});
    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.renderCache().clear();
    window.monitor()->update();
    QApplication::processEvents();
}

// Comparison view, through the real monitor.
//
// A lit frame held as the reference while the playhead sits on a dark
// one: with the split in the middle, half the screen has to be lit and
// half dark, and moving the split has to move where the boundary is.
TEST_CASE("The comparison view splits the picture", "[gui]") {
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

    const auto cmpRate =
        window.project().findSequence(window.project().activeSequence())->frameRate();

    std::int64_t litFrame = -1;
    std::int64_t darkFrame = -1;
    double lit = 0.0;
    double darkest = 1e9;
    for (std::int64_t frame = 0; frame < 40; ++frame) {
        window.setPosition(zaro::time::RationalTime{frame, cmpRate});
        const double gray = meanGray(settledGrab(window.monitor()));
        if (gray > lit) {
            lit = gray;
            litFrame = frame;
        }
        if (gray < darkest) {
            darkest = gray;
            darkFrame = frame;
        }
    }
    if (litFrame < 0 || darkFrame < 0 || !(lit > darkest + 40.0)) {
        zaro::app::testing::failf("no lit and dark pair to compare\n");
    }

    // Hold the lit frame, then look at the dark one against it.
    window.setPosition(zaro::time::RationalTime{litFrame, cmpRate});
    QApplication::processEvents();
    window.setComparing(true, zaro::time::RationalTime{litFrame, cmpRate});
    window.setPosition(zaro::time::RationalTime{darkFrame, cmpRate});
    QApplication::processEvents();

    const QImage halves = settledGrab(window.monitor());
    const double leftHalf = meanGray(halves.copy(0, 0, halves.width() / 2, halves.height()));
    const double rightHalf =
        meanGray(halves.copy(halves.width() / 2, 0, halves.width() / 2, halves.height()));
    std::printf("  comparison: %.1f on the reference side, %.1f on the current one\n", leftHalf,
                rightHalf);
    if (!(leftHalf > rightHalf + 30.0)) {
        zaro::app::testing::failf("the reference frame is not showing on its own side\n");
    }

    // Move the split most of the way over and the reference takes over.
    window.setCompareSplit(0.95);
    QApplication::processEvents();
    const double mostlyReference = meanGray(settledGrab(window.monitor()));
    if (!(mostlyReference > leftHalf * 0.8)) {
        zaro::app::testing::failf("moving the split did not move the boundary\n");
    }

    // And switching it off puts the plain frame back.
    window.setComparing(false, zaro::time::RationalTime{litFrame, cmpRate});
    window.setCompareSplit(0.5);
    QApplication::processEvents();
    const double plain = meanGray(settledGrab(window.monitor()));
    if (!(plain < leftHalf * 0.5)) {
        zaro::app::testing::failf("turning comparison off did not restore the frame\n");
    }
}

// Baking a look out as a .cube, and reading it back.
//
// The round trip is the assertion: a grade written to a file and loaded
// through the reader has to land on the same colour, or the look
// somebody hands to a colourist is not the one they approved.
TEST_CASE("A look baked out as a .cube and read back", "[gui]") {
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

    const auto bakeSequenceId = window.project().activeSequence();
    const auto bakeTrackId =
        window.project().findSequence(bakeSequenceId)->videoTracks().front().id();
    const auto bakeClipId =
        window.project().findSequence(bakeSequenceId)->findTrack(bakeTrackId)->clips().front().id;

    zaro::model::ColorCorrection look;
    look.saturation = 35.0;
    // Deliberately not a white balance shift: warming the picture
    // multiplies the red channel, so pure white lands above one and the
    // bake correctly reports that the cube has nowhere to put it. A
    // grade that only pulls values down stays inside the domain, which
    // is the case this block is checking.
    look.exposure = -0.3;
    auto graded = zaro::edit::makeSetColorCorrection(
        window.project(), {bakeSequenceId, bakeTrackId}, bakeClipId, look);
    if (!graded) {
        zaro::app::testing::failf("%s\n", graded.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*graded));

    const auto* bakeClip =
        window.project().findSequence(bakeSequenceId)->findTrack(bakeTrackId)->find(bakeClipId);
    zaro::render::LutOmissions omissions;
    auto text = zaro::render::bakeCube(
        *bakeClip, window.project().findSequence(bakeSequenceId)->output().transfer, 33, "selftest",
        &omissions);
    if (!text) {
        zaro::app::testing::failf("%s\n", text.error().toString().c_str());
    }
    auto parsed = zaro::io::CubeLut::parse(*text);
    if (!parsed) {
        zaro::app::testing::failf("the baked cube does not parse: %s\n",
                                  parsed.error().toString().c_str());
    }

    // The same colour through the grade and through the file.
    const auto transfer = window.project().findSequence(bakeSequenceId)->output().transfer;
    const auto grade = zaro::render::gradeConstantsFor(bakeClip->color, bakeClip->wheels);
    float r = zaro::render::toLinearScalar(0.6F, transfer);
    float g = zaro::render::toLinearScalar(0.35F, transfer);
    float b = zaro::render::toLinearScalar(0.2F, transfer);
    zaro::render::gradePixel(grade, r, g, b, nullptr, nullptr, nullptr, 1.0F);
    const float wantR = zaro::render::fromLinearScalar(r, transfer);

    float lr = 0.6F;
    float lg = 0.35F;
    float lb = 0.2F;
    parsed->apply(lr, lg, lb);

    std::printf("  look file: %d entries a side, graded red %.3f, through the cube %.3f\n",
                parsed->size(), static_cast<double>(wantR), static_cast<double>(lr));
    if (std::fabs(static_cast<double>(lr) - static_cast<double>(wantR)) > 0.01) {
        zaro::app::testing::failf("the baked look does not match the grade\n");
    }
    if (omissions.any()) {
        zaro::app::testing::failf("a plain grade reported something left out: %s\n",
                                  omissions.describe().c_str());
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.monitor()->update();
    QApplication::processEvents();
}

// The vignette, through the panel and out to the picture. A vignette
// darkens the corners without making them transparent, so on a frame
// that is lit the mean brightness has to fall while the frame stays
// opaque.
TEST_CASE("A vignette pulls the corners down", "[gui]") {
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

    const auto vigSequenceId = window.project().activeSequence();
    const auto vigTrackId =
        window.project().findSequence(vigSequenceId)->videoTracks().front().id();
    const auto vigClipId =
        window.project().findSequence(vigSequenceId)->findTrack(vigTrackId)->clips().front().id;
    const auto vigRate = window.project().findSequence(vigSequenceId)->frameRate();

    timeline->selectOnly(vigTrackId, vigClipId);
    window.effects()->setSelection(vigTrackId, vigClipId);

    std::int64_t litFrame = 0;
    double lit = 0.0;
    for (std::int64_t frame = 0; frame < 40; ++frame) {
        window.setPosition(zaro::time::RationalTime{frame, vigRate});
        const double gray = meanGray(settledGrab(window.monitor()));
        if (gray > lit) {
            lit = gray;
            litFrame = frame;
        }
    }
    window.setPosition(zaro::time::RationalTime{litFrame, vigRate});
    const double open = meanGray(settledGrab(window.monitor()));

    auto* amount = window.effects()->findChild<QDoubleSpinBox*>("vignette-amount");
    if (amount == nullptr) {
        zaro::app::testing::failf("the vignette control is not in the panel\n");
    }
    amount->setValue(-1.0);
    QApplication::processEvents();

    const auto* vignetted =
        window.project().findSequence(vigSequenceId)->findTrack(vigTrackId)->find(vigClipId);
    if (vignetted == nullptr || !vignetted->vignette.isSet()) {
        zaro::app::testing::failf("the vignette did not reach the clip\n");
    }
    const double darkened = meanGray(settledGrab(window.monitor()));
    std::printf("  vignette: %.1f open, %.1f with the corners pulled down\n", open, darkened);
    if (!(darkened < open * 0.9)) {
        zaro::app::testing::failf("the vignette did not reach the picture\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.monitor()->update();
    QApplication::processEvents();
}

// A look LUT, loaded from a real file through the model the panel
// writes to. The parser and the baked cube are tested headlessly and
// the two render paths are compared; what is left is whether a LUT set
// on a clip reaches the picture at all.
TEST_CASE("A look LUT loaded from a real file", "[gui]") {
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

    const auto clipNow = [&]() {
        return window.project()
            .findSequence(sequence.id())
            ->videoTracks()
            .front()
            .find(original.id);
    };
    // A dark frame: this look lifts black by 0.15, which a black frame
    // shows and a white one cannot.
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
    QApplication::processEvents();
    const double plainDark = meanGray(settledGrab(window.monitor()));

    // Written here, for the same reason the captions are: a fixture in
    // somebody's scratch folder is a test that quietly stops running.
    // A 2x2x2 cube that lifts black by 0.15 and warms the highlights,
    // which is the smallest thing that changes a picture visibly.
    const std::filesystem::path lutPath =
        std::filesystem::temp_directory_path() / "zaro-selftest-warm.cube";
    {
        std::ofstream writing{lutPath};
        writing << "TITLE \"warm lift\"\nLUT_3D_SIZE 2\n";
        for (int blue = 0; blue < 2; ++blue) {
            for (int green = 0; green < 2; ++green) {
                for (int red = 0; red < 2; ++red) {
                    const double r = 0.75 + (red * 0.25);
                    const double g = 0.72 + (green * 0.28);
                    const double b = 0.68 + (blue * 0.32);
                    writing << r << " " << g << " " << b << "\n";
                }
            }
        }
    }
    zaro::model::LutRef look;
    look.path = lutPath.string();
    auto built = zaro::edit::makeSetLut(window.project(), {sequence.id(), videoTrack.id()},
                                        original.id, look);
    if (!built) {
        zaro::app::testing::failf("%s\n", built.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*built));
    window.monitor()->update();
    QApplication::processEvents();
    const double lifted = meanGray(settledGrab(window.monitor()));

    // And dialled back to nothing, which has to return the picture.
    zaro::model::LutRef none = clipNow()->lut;
    none.amount = 0.0;
    auto cleared = zaro::edit::makeSetLut(window.project(), {sequence.id(), videoTrack.id()},
                                          original.id, none);
    window.commands().execute(window.project(), std::move(*cleared));
    window.monitor()->update();
    QApplication::processEvents();
    const double off = meanGray(settledGrab(window.monitor()));

    std::printf(
        "  look LUT: %.1f before, %.1f applied, %.1f at zero amount "
        "(a white frame reads %.1f)\n",
        plainDark, lifted, off, brightest);
    // The fixture lifts black to three quarters of white, so the lifted
    // frame should read most of what a white frame reads -- stated
    // against that frame rather than against a number.
    // A third of a white frame, not half: the measured ratio is about
    // 0.57, and a threshold sitting just under the value it checks is
    // a test that will fail for a reason nobody wants to investigate.
    if (!(lifted > brightest * 0.3) || !(lifted > plainDark + 1.0)) {
        zaro::app::testing::failf("the look LUT did not reach the picture\n");
    }
    if (std::fabs(off - plainDark) > 1.0) {
        zaro::app::testing::failf("an amount of zero still changed the picture\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.monitor()->update();
    QApplication::processEvents();
}

// The secondary, through its panel. The qualifier is tested headlessly
// and compared against the shader; what neither of those can see is
// whether these controls are connected to any of it.
TEST_CASE("The secondary, through its panel", "[gui]") {
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

    auto* enable = window.effects()->findChild<QCheckBox*>("qualifier-enabled");
    auto* mask = window.effects()->findChild<QCheckBox*>("qualifier-show-mask");
    auto* lumaHigh = window.effects()->findChild<QDoubleSpinBox*>("qualifier-luma-high");
    if (enable == nullptr || mask == nullptr || lumaHigh == nullptr) {
        zaro::app::testing::failf("the qualifier controls are missing\n");
    }
    window.effects()->setSelection(videoTrack.id(), original.id);
    QApplication::processEvents();

    // A lit frame, so "selected" and "not selected" are a white mask
    // and a black one rather than two black pictures.
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
    // The picture itself, to compare the mask against. An absolute
    // threshold would be measuring the letterbox: how much of the
    // monitor the picture covers depends on the panel layout, and that
    // changes whenever a control is added.
    const double picture = meanGray(settledGrab(window.monitor()));

    enable->setChecked(true);
    mask->setChecked(true);
    window.monitor()->update();
    QApplication::processEvents();
    const double everything = meanGray(settledGrab(window.monitor()));

    // Now key only the darks. This frame is white, so it drops out.
    lumaHigh->setValue(0.2);
    window.monitor()->update();
    QApplication::processEvents();
    const double nothing = meanGray(settledGrab(window.monitor()));

    std::printf("  qualifier mask: picture %.1f, wide open %.1f, keyed to darks %.1f\n", picture,
                everything, nothing);
    // A white picture, entirely selected, shows as a white mask -- so
    // the two readings should agree.
    if (!(everything > picture * 0.85)) {
        zaro::app::testing::failf(
            "a qualifier left wide open did not select the picture "
            "(%.1f against %.1f)\n",
            everything, picture);
    }
    if (!(nothing < everything * 0.2)) {
        zaro::app::testing::failf(
            "narrowing the luma window did not change the mask; the "
            "controls are not reaching the compositor\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.effects()->refresh();
    window.monitor()->update();
    QApplication::processEvents();
}

// An adjustment layer, through the real preview. This is the one
// feature where the GPU path deliberately hands the whole frame to the
// CPU compositor, so what this checks is that the fallback is wired and
// that what it produces reaches the screen.
TEST_CASE("An adjustment layer, through the real preview", "[gui]") {
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

    const auto adjustSequenceId = sequence.id();
    // This fixture has one video track, and an adjustment layer needs
    // something to sit above.
    if (window.project().findSequence(adjustSequenceId)->videoTracks().size() < 2) {
        auto added = zaro::edit::makeAddTrack(window.project(), adjustSequenceId,
                                              zaro::model::TrackKind::Video, "V2");
        if (!added) {
            zaro::app::testing::failf("%s\n", added.error().toString().c_str());
        }
        window.commands().execute(window.project(), std::move(*added));
    }
    const auto aboveId = window.project().findSequence(adjustSequenceId)->videoTracks()[1].id();

    std::int64_t brightestFrame = 0;
    double brightest = 0.0;
    for (std::int64_t frame = 0; frame < 40; ++frame) {
        window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
        const double gray = meanGray(settledGrab(window.monitor()));
        if (gray > brightest) {
            brightest = gray;
            brightestFrame = frame;
        }
    }
    window.setPosition(zaro::time::RationalTime{brightestFrame, sequence.frameRate()});
    QApplication::processEvents();
    const double plain = meanGray(settledGrab(window.monitor()));

    auto built = zaro::edit::makeAddAdjustment(
        window.project(), {adjustSequenceId, aboveId},
        zaro::time::TimeRange{zaro::time::RationalTime{0, sequence.frameRate()},
                              zaro::time::RationalTime{60, sequence.frameRate()}});
    if (!built) {
        zaro::app::testing::failf("%s\n", built.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*built));
    const auto layerId =
        window.project().findSequence(adjustSequenceId)->findTrack(aboveId)->clips().front().id;

    zaro::model::ColorCorrection darker;
    darker.exposure = -2.0;
    auto graded = zaro::edit::makeSetColorCorrection(window.project(), {adjustSequenceId, aboveId},
                                                     layerId, darker);
    if (!graded) {
        zaro::app::testing::failf("%s\n", graded.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*graded));
    window.monitor()->update();
    QApplication::processEvents();
    const double adjusted = meanGray(settledGrab(window.monitor()));

    std::printf("  adjustment layer: %.1f plain, %.1f two stops down from above\n", plain,
                adjusted);
    if (!(adjusted < plain * 0.5)) {
        zaro::app::testing::failf("the adjustment layer did not reach the preview\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.monitor()->update();
    QApplication::processEvents();
}
