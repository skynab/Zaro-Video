// Time: retimes, transitions, and the edits that move the picture.
//
// Driven through the real window against the real compositor. See GuiFixture.h
// for what is shared and why.

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "../FrameGrab.h"
#include "GuiFixture.h"

// The suite was written inside main(), which had this at file scope; the
// bodies still say `model::` and `Status` unqualified.
using namespace zaro;

using zaro::app::dragOnTimeline;
using zaro::app::settledGrab;

// Time remapping and freeze frames, through the real panel.
//
// A freeze is the one retime whose effect is visible in a single frame:
// a frame that was black shows the lit one instead. That makes it
// measurable here in a way a speed ramp is not, and it exercises the
// same curve a ramp would use.
TEST_CASE("Time remapping and freeze frames", "[gui]") {
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

    const auto grayAt = [&](std::int64_t frame) {
        window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
        return meanGray(settledGrab(window.monitor()));
    };

    std::int64_t litFrame = -1;
    double lit = 0.0;
    std::int64_t darkFrame = -1;
    double darkest = 1e9;
    for (std::int64_t frame = 8; frame < 60; ++frame) {
        const double gray = grayAt(frame);
        if (gray > lit) {
            lit = gray;
            litFrame = frame;
        }
        if (gray < darkest) {
            darkest = gray;
            darkFrame = frame;
        }
    }
    if (litFrame < 0 || darkFrame < 0 || !(lit > darkest * 4.0 + 10.0)) {
        zaro::app::testing::failf("no lit and dark pair to freeze between\n");
    }

    const auto remapTrackId =
        window.project().findSequence(sequence.id())->videoTracks().front().id();
    const auto remapClipId =
        window.project().findSequence(sequence.id())->findTrack(remapTrackId)->clips().front().id;
    timeline->selectOnly(remapTrackId, remapClipId);
    window.effects()->setSelection(remapTrackId, remapClipId);
    window.setPosition(zaro::time::RationalTime{litFrame, sequence.frameRate()});
    QApplication::processEvents();

    auto* freezeButton = window.effects()->findChild<QPushButton*>("freeze-frame");
    auto* remapBox = window.effects()->findChild<QCheckBox*>("time-remap");
    if (freezeButton == nullptr || remapBox == nullptr) {
        zaro::app::testing::failf("the time remap controls are not in the panel\n");
    }
    freezeButton->click();
    QApplication::processEvents();

    if (!remapBox->isChecked()) {
        zaro::app::testing::failf("freezing did not turn time remapping on\n");
    }
    // The frame that was black now shows the lit one.
    const double frozen = grayAt(darkFrame);
    std::printf(
        "  freeze frame: %.1f lit, %.1f dark, %.1f after freezing on the lit "
        "frame\n",
        lit, darkest, frozen);
    if (!(frozen > lit * 0.8)) {
        zaro::app::testing::failf("the freeze did not reach the picture\n");
    }

    // And switching it off puts the clip back on its own frames.
    remapBox->setChecked(false);
    QApplication::processEvents();
    const double thawed = grayAt(darkFrame);
    if (!(thawed < lit * 0.5)) {
        zaro::app::testing::failf("removing the remap did not restore the clip\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.monitor()->update();
    QApplication::processEvents();
}

// Responsive timing, through the real panel and a real trim.
//
// A title that fades up and away, trimmed shorter: without the
// protection the exit is simply cut off, and the point of the feature
// is that the last frame goes dark either way.
TEST_CASE("Responsive timing survives a trim", "[gui]") {
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

    const auto respSequenceId = window.project().activeSequence();
    const auto* respSequence = window.project().findSequence(respSequenceId);
    const auto respRate = respSequence->frameRate();
    const auto respTrackId = respSequence->videoTracks().back().id();
    constexpr int kAuthored = 48;
    constexpr int kTrimmed = 24;

    zaro::model::Graphic card;
    card.kind = zaro::model::GraphicKind::Rectangle;
    card.width = 200.0;
    card.height = 150.0;
    card.red = 1.0;
    card.green = 1.0;
    card.blue = 1.0;
    auto added = zaro::edit::makeAddGraphic(
        window.project(), {respSequenceId, respTrackId}, card,
        zaro::time::TimeRange{zaro::time::RationalTime{0, respRate},
                              zaro::time::RationalTime{kAuthored, respRate}});
    if (!added) {
        zaro::app::testing::failf("%s\n", added.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*added));
    zaro::model::ClipId cardId;
    for (const auto& candidate :
         window.project().findSequence(respSequenceId)->findTrack(respTrackId)->clips()) {
        if (candidate.graphic.kind == zaro::model::GraphicKind::Rectangle &&
            candidate.graphic.width == 200.0) {
            cardId = candidate.id;
        }
    }
    if (!cardId.isValid()) {
        zaro::app::testing::failf("the card was not added\n");
    }

    // Up over twelve frames, hold, away over the last twelve.
    zaro::model::Curve fade;
    for (const auto& [frame, value] :
         {std::pair{0, 0.0}, std::pair{12, 1.0}, std::pair{36, 1.0}, std::pair{kAuthored, 0.0}}) {
        fade.set(zaro::model::Keyframe{zaro::time::RationalTime{frame, respRate},
                                       value,
                                       zaro::model::Interpolation::Linear,
                                       {},
                                       {}});
    }
    auto animated = zaro::edit::makeSetCurve(window.project(), {respSequenceId, respTrackId},
                                             cardId, zaro::model::Param::Opacity, fade);
    if (!animated) {
        zaro::app::testing::failf("%s\n", animated.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*animated));

    timeline->selectOnly(respTrackId, cardId);
    window.effects()->setSelection(respTrackId, cardId);
    QApplication::processEvents();
    auto* introBox = window.effects()->findChild<QDoubleSpinBox*>("responsive-intro");
    auto* outroBox = window.effects()->findChild<QDoubleSpinBox*>("responsive-outro");
    if (introBox == nullptr || outroBox == nullptr) {
        zaro::app::testing::failf("the responsive controls are not in the panel\n");
    }
    const double half = 12.0 / respRate.toDouble();
    introBox->setValue(half);
    outroBox->setValue(half);
    QApplication::processEvents();
    const auto* protectedCard =
        window.project().findSequence(respSequenceId)->findTrack(respTrackId)->find(cardId);
    if (!protectedCard->responsive.isSet() ||
        protectedCard->responsive.authored.frames() != kAuthored) {
        zaro::app::testing::failf("the panel did not set the responsive timing\n");
    }

    // Trim the tail, the way a trim tool does.
    auto cut = zaro::edit::makeTrim(window.project(), {respSequenceId, respTrackId}, cardId,
                                    zaro::edit::Edge::Out,
                                    zaro::time::RationalTime{kTrimmed - kAuthored, respRate});
    if (!cut) {
        zaro::app::testing::failf("%s\n", cut.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*cut));

    const auto* trimmedCard =
        window.project().findSequence(respSequenceId)->findTrack(respTrackId)->find(cardId);
    const auto endFrame = trimmedCard->endExclusive() - zaro::time::RationalTime{1, respRate};
    const double lastOpacity = trimmedCard->transformAt(endFrame).opacity;
    const double midOpacity =
        trimmedCard->transformAt(trimmedCard->start() + zaro::time::RationalTime{12, respRate})
            .opacity;
    std::printf("  responsive timing: %.2f opacity mid-clip, %.2f on the last frame\n", midOpacity,
                lastOpacity);
    if (!(midOpacity > 0.9)) {
        zaro::app::testing::failf("the protected intro did not finish\n");
    }
    if (!(lastOpacity < 0.2)) {
        zaro::app::testing::failf("the exit did not follow the trim\n");
    }

    // And on the picture: the last frame of a title that has faded out
    // is darker than the middle of it.
    window.setPosition(trimmedCard->start() + zaro::time::RationalTime{12, respRate});
    window.renderCache().clear();
    const double litMiddle = meanGray(settledGrab(window.monitor()));
    window.setPosition(endFrame);
    window.renderCache().clear();
    const double litEnd = meanGray(settledGrab(window.monitor()));
    if (!(litEnd < litMiddle)) {
        zaro::app::testing::failf("the faded-out last frame is not darker (%.1f vs %.1f)\n", litEnd,
                                  litMiddle);
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.renderCache().clear();
    window.monitor()->update();
    QApplication::processEvents();
}

// Wipes and slides, through the real timeline and the real compositor.
//
// Between two generated clips rather than the footage: this fixture is
// black except on its flash frames, and a wipe between two black shots
// is a measurement of nothing. A white rectangle and a grey one have a
// boundary somebody can point at, which is exactly what a wipe is for.
TEST_CASE("Wipes and slides, through the real compositor", "[gui]") {
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

    const auto wipeSequenceId = window.project().activeSequence();
    const auto& wipeTracks = window.project().findSequence(wipeSequenceId)->videoTracks();
    const auto wipeTrackId = wipeTracks.size() > 1 ? wipeTracks[1].id() : wipeTracks.front().id();
    const auto wipeRate = window.project().findSequence(wipeSequenceId)->frameRate();

    zaro::model::Graphic panel;
    panel.kind = zaro::model::GraphicKind::Rectangle;
    panel.width = 4000.0;
    panel.height = 4000.0;
    panel.red = 1.0;
    panel.green = 1.0;
    panel.blue = 1.0;
    auto added =
        zaro::edit::makeAddGraphic(window.project(), {wipeSequenceId, wipeTrackId}, panel,
                                   zaro::time::TimeRange{zaro::time::RationalTime{0, wipeRate},
                                                         zaro::time::RationalTime{80, wipeRate}});
    if (!added) {
        zaro::app::testing::failf("%s\n", added.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*added));

    const auto cutAt = zaro::time::RationalTime{40, wipeRate};
    auto razored = zaro::edit::makeRazor(window.project(), {wipeSequenceId, wipeTrackId}, cutAt);
    if (!razored) {
        zaro::app::testing::failf("%s\n", razored.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*razored));

    // The second piece pulled well down, so the two shots differ.
    // The piece that starts at the cut, not the last clip on the
    // track -- the fixture's own clip is still there beyond the panel.
    const zaro::model::Clip* tail =
        window.project().findSequence(wipeSequenceId)->findTrack(wipeTrackId)->clipAt(cutAt);
    if (tail == nullptr) {
        zaro::app::testing::failf("the razor left nothing at the cut\n");
    }
    const auto tailId = tail->id;
    zaro::model::ColorCorrection dark;
    dark.exposure = -4.0;
    auto graded = zaro::edit::makeSetColorCorrection(window.project(),
                                                     {wipeSequenceId, wipeTrackId}, tailId, dark);
    if (!graded) {
        zaro::app::testing::failf("%s\n", graded.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*graded));

    auto dissolve =
        zaro::edit::makeAddCrossDissolve(window.project(), {wipeSequenceId, wipeTrackId}, cutAt,
                                         zaro::time::RationalTime{20, wipeRate});
    if (!dissolve) {
        zaro::app::testing::failf("%s\n", dissolve.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*dissolve));

    const auto span = window.project()
                          .findSequence(wipeSequenceId)
                          ->findTrack(wipeTrackId)
                          ->transitions()
                          .front()
                          .range;
    const auto middle =
        span.start() + zaro::time::RationalTime{span.duration().frames() / 2, span.start().rate()};

    timeline->selectOnly(
        wipeTrackId,
        window.project().findSequence(wipeSequenceId)->findTrack(wipeTrackId)->clips().front().id);
    window.setPosition(middle);
    window.renderCache().clear();
    const QImage blended = settledGrab(window.monitor());
    const double dissolveLeft = meanGray(blended.copy(0, 0, blended.width() / 2, blended.height()));
    const double dissolveRight =
        meanGray(blended.copy(blended.width() / 2, 0, blended.width() / 2, blended.height()));

    if (!timeline->setTransitionKindAtPlayhead(zaro::model::TransitionKind::Wipe,
                                               zaro::model::TransitionDirection::Right)) {
        zaro::app::testing::failf("the transition kind could not be changed\n");
    }
    window.renderCache().clear();
    const QImage wiped = settledGrab(window.monitor());
    const double wipeLeft = meanGray(wiped.copy(0, 0, wiped.width() / 2, wiped.height()));
    const double wipeRight =
        meanGray(wiped.copy(wiped.width() / 2, 0, wiped.width() / 2, wiped.height()));

    std::printf("  wipe: dissolve halves %.1f/%.1f, wipe halves %.1f/%.1f\n", dissolveLeft,
                dissolveRight, wipeLeft, wipeRight);
    // A dissolve blends both halves the same way; a wipe puts one shot
    // on each side of a line.
    if (std::fabs(dissolveLeft - dissolveRight) > 10.0) {
        zaro::app::testing::failf("a dissolve did not blend evenly across the frame\n");
    }
    if (!(wipeRight > wipeLeft + 30.0)) {
        zaro::app::testing::failf("the wipe did not put the two shots either side\n");
    }

    if (!timeline->setTransitionKindAtPlayhead(zaro::model::TransitionKind::Slide,
                                               zaro::model::TransitionDirection::Right)) {
        zaro::app::testing::failf("the transition could not be made a slide\n");
    }
    window.renderCache().clear();
    static_cast<void>(settledGrab(window.monitor()));
    if (!window.monitor()->lastError().isEmpty()) {
        zaro::app::testing::failf("rendering a slide reported %s\n",
                                  window.monitor()->lastError().toUtf8().constData());
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.renderCache().clear();
    window.monitor()->update();
    QApplication::processEvents();
}

// Scene edit detection, over the real footage.
//
// This fixture is one continuous take with white flashes in it, which
// makes it exactly the case the detector has to get right: a flash is
// not a cut. The assertion is that it finds *nothing*, and it can fail
// -- with the flash guard removed the same clip comes back in pieces.
TEST_CASE("Scene edit detection over the real footage", "[gui]") {
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

    const auto sceneSequenceId = window.project().activeSequence();
    const auto sceneTrackId =
        window.project().findSequence(sceneSequenceId)->videoTracks().front().id();
    const auto* sceneTrack =
        window.project().findSequence(sceneSequenceId)->findTrack(sceneTrackId);
    if (sceneTrack->clips().empty()) {
        zaro::app::testing::failf("nothing on the track to analyse\n");
    }
    const auto sceneClipId = sceneTrack->clips().front().id;
    const std::size_t clipsBefore = sceneTrack->clips().size();

    timeline->selectOnly(sceneTrackId, sceneClipId);
    window.effects()->setSelection(sceneTrackId, sceneClipId);
    QApplication::processEvents();

    const std::int32_t found = window.detectScenes();
    const std::size_t clipsAfter =
        window.project().findSequence(sceneSequenceId)->findTrack(sceneTrackId)->clips().size();
    std::printf(
        "  scene detection: %d cuts in a continuous take, %zu clips before and "
        "%zu after\n",
        found, clipsBefore, clipsAfter);
    if (found != 0 || clipsAfter != clipsBefore) {
        zaro::app::testing::failf("a take with flashes in it was cut into pieces\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
}

// Text-based editing: delete words, and the picture goes with them.
TEST_CASE("Text-based editing takes the picture with it", "[gui]") {
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

    const auto textSequenceId = window.project().activeSequence();
    const auto textRate = window.project().findSequence(textSequenceId)->frameRate();
    const auto textTrack =
        window.project().findSequence(textSequenceId)->videoTracks().front().id();

    // A transcript over whatever is on the timeline, with a filler word
    // in one line of it.
    zaro::model::CaptionTrack said;
    const struct Line {
        std::int64_t from;
        std::int64_t frames;
        const char* text;
    } lines[] = {
        {0, 20, "so here we are"}, {20, 20, "um the number is nine"}, {40, 20, "and that is that"}};
    for (const Line& line : lines) {
        zaro::model::Caption caption;
        caption.range = zaro::time::TimeRange{zaro::time::RationalTime{line.from, textRate},
                                              zaro::time::RationalTime{line.frames, textRate}};
        caption.text = line.text;
        said.add(caption);
    }
    auto captioned = zaro::edit::makeSetCaptions(window.project(), textSequenceId, said);
    if (!captioned) {
        zaro::app::testing::failf("%s\n", captioned.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*captioned));

    auto* transcript = window.showTranscript();
    if (transcript == nullptr || transcript->lineCount() != 3) {
        zaro::app::testing::failf("the transcript shows %d lines, not three\n",
                                  transcript == nullptr ? -1 : transcript->lineCount());
    }

    // "Select filler" finds the line with "um" in it, and not the one
    // with "number": a substring test would take both, and cutting a
    // line because it mentions a number is the kind of mistake nobody
    // would forgive a transcript tool.
    const int filler = transcript->selectContaining({"um", "uh"});
    if (filler != 1) {
        zaro::app::testing::failf("filler selection matched %d lines, not one\n", filler);
    }

    const auto* beforeTrack = window.project().findSequence(textSequenceId)->findTrack(textTrack);
    const auto lengthBefore = beforeTrack->clips().back().endExclusive();

    auto gone = transcript->deleteSelected();
    if (!gone || *gone != 1) {
        zaro::app::testing::failf("deleting the filler line removed %d lines\n", gone ? *gone : -1);
    }
    const auto* afterTrack = window.project().findSequence(textSequenceId)->findTrack(textTrack);
    const auto lengthAfter = afterTrack->clips().back().endExclusive();
    std::printf("  text editing: %lld frames became %lld, %d lines left\n",
                static_cast<long long>(lengthBefore.frames()),
                static_cast<long long>(lengthAfter.frames()), transcript->lineCount());

    if (lengthAfter != lengthBefore - zaro::time::RationalTime{20, textRate}) {
        zaro::app::testing::failf("the picture did not shorten with the words\n");
    }
    if (transcript->lineCount() != 2) {
        zaro::app::testing::failf("the transcript still shows %d lines\n", transcript->lineCount());
    }
    // And what followed moved up: the last line now starts where the
    // deleted one did.
    const auto& left = window.project().findSequence(textSequenceId)->captions().captions();
    if (left.size() != 2 || left.back().range.start().frames() != 20) {
        zaro::app::testing::failf("the remaining transcript did not move up\n");
    }
    // One undo puts the words and the picture back together.
    window.commands().undo(window.project());
    transcript->refresh();
    const auto* undoneTrack = window.project().findSequence(textSequenceId)->findTrack(textTrack);
    if (undoneTrack->clips().back().endExclusive() != lengthBefore ||
        transcript->lineCount() != 3) {
        zaro::app::testing::failf("undo did not put both back\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    transcript->refresh();
    QApplication::processEvents();
}

// Fitting music to a length, on a track with a known beat.
TEST_CASE("Fitting music to a length", "[gui]") {
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

    auto probedClick =
        zaro::platform::ffmpeg::probe(zaro::app::testing::mediaFixture("click_track.wav"));
    if (!probedClick) {
        zaro::app::testing::failf("%s (run testdata/generate.sh)\n",
                                  probedClick.error().toString().c_str());
    }
    zaro::model::MediaRef music;
    music.path = zaro::app::testing::mediaFixture("click_track.wav");
    music.name = "click track";
    music.info = *probedClick;
    auto broughtIn = zaro::edit::makeImportMedia(window.project(), music);
    if (!broughtIn) {
        zaro::app::testing::failf("%s\n", broughtIn.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*broughtIn));
    const auto musicId = window.project().media().back().id;
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }

    const auto musicSequenceId = window.project().activeSequence();
    const auto* musicSequence = window.project().findSequence(musicSequenceId);
    const auto musicRate = musicSequence->frameRate();
    const auto musicTrack = musicSequence->audioTracks().front().id();
    auto laid = zaro::edit::makePlaceFromSource(
        window.project(), {musicSequenceId, musicTrack}, musicId,
        zaro::time::TimeRange{zaro::time::RationalTime{0, musicRate},
                              zaro::time::RationalTime{
                                  static_cast<std::int64_t>(12 * musicRate.toDouble()), musicRate}},
        zaro::time::RationalTime{0, musicRate}, zaro::edit::PlaceMode::Overwrite);
    if (!laid) {
        zaro::app::testing::failf("%s\n", laid.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*laid));
    zaro::model::ClipId musicClip;
    for (const auto& candidate :
         window.project().findSequence(musicSequenceId)->findTrack(musicTrack)->clips()) {
        if (candidate.source == musicId) {
            musicClip = candidate.id;
        }
    }
    if (!musicClip.isValid()) {
        zaro::app::testing::failf("the music was not placed\n");
    }

    timeline->selectOnly(musicTrack, musicClip);
    window.effects()->setSelection(musicTrack, musicClip);
    QApplication::processEvents();

    constexpr double kWanted = 7.0;
    auto plan = window.remixSelectedTo(kWanted);
    if (!plan) {
        zaro::app::testing::failf("%s\n", plan.error().toString().c_str());
    }
    std::printf("  fit music: cut %d beats at %.2fs, %.2fs long\n", plan->beatsRemoved, plan->cutAt,
                plan->seconds);
    // Within a beat of what was asked for: landing exactly would mean
    // cutting off the beat.
    if (std::fabs(plan->seconds - kWanted) > 0.5) {
        zaro::app::testing::failf("the remix is %.2fs, not %.2fs\n", plan->seconds, kWanted);
    }
    // The cut lands on a click, not between them.
    const double intoBeat = std::fmod(plan->cutAt + 0.005, 0.5);
    if (std::min(intoBeat, 0.5 - intoBeat) > 0.05) {
        zaro::app::testing::failf("the cut at %.3fs is not on a beat\n", plan->cutAt);
    }

    // Two clips now, with the join crossfaded rather than butted.
    const auto* remixed = window.project().findSequence(musicSequenceId)->findTrack(musicTrack);
    int pieces = 0;
    bool hasFade = false;
    double covered = 0.0;
    for (const auto& piece : remixed->clips()) {
        if (piece.source != musicId) {
            continue;
        }
        ++pieces;
        covered = std::max(covered, piece.endExclusive().toSecondsDouble());
        hasFade = hasFade || piece.animation.find(zaro::model::Param::GainDb) != nullptr;
    }
    if (pieces != 2) {
        zaro::app::testing::failf("the remix made %d pieces, not two\n", pieces);
    }
    if (!hasFade) {
        zaro::app::testing::failf("the join has no fade on it\n");
    }
    if (std::fabs(covered - plan->seconds) > 0.2) {
        zaro::app::testing::failf("the timeline holds %.2fs, not %.2fs\n", covered, plan->seconds);
    }
    // Asking for more than there is says so rather than looping.
    if (auto longer = window.remixSelectedTo(30.0); longer) {
        zaro::app::testing::failf("making the music longer was allowed\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    QApplication::processEvents();
}

// Auto-reframe, on a sequence whose shape does not match the footage.
TEST_CASE("Auto-reframe for a different shape", "[gui]") {
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

    const auto originalSequenceId = window.project().activeSequence();
    // A tall sequence, which is what reframing is for: 320x240 footage
    // in a 240x320 frame has to be scaled and then aimed.
    zaro::model::Sequence tall{window.project().ids().next<zaro::model::SequenceTag>(), "Vertical",
                               zaro::time::rates::fps25};
    tall.setSize(240, 320);
    const auto tallId = tall.id();
    const auto tallTrack = window.project().ids().next<zaro::model::TrackTag>();
    tall.addTrack(tallTrack, zaro::model::TrackKind::Video, "V1");
    window.project().addSequence(std::move(tall));
    window.rebindSequence();

    const auto& shakyMedia = window.project().media().front();
    auto placed = zaro::edit::makePlaceFromSource(
        window.project(), {tallId, tallTrack}, shakyMedia.id,
        zaro::time::TimeRange{zaro::time::RationalTime{0, zaro::time::rates::fps25},
                              zaro::time::RationalTime{10, zaro::time::rates::fps25}},
        zaro::time::RationalTime{0, zaro::time::rates::fps25}, zaro::edit::PlaceMode::Overwrite);
    if (!placed) {
        zaro::app::testing::failf("%s\n", placed.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*placed));
    const auto tallClip =
        window.project().findSequence(tallId)->findTrack(tallTrack)->clips().front().id;

    window.setActiveSequence(tallId);
    timeline->selectOnly(tallTrack, tallClip);
    window.effects()->setSelection(tallTrack, tallClip);
    QApplication::processEvents();

    auto* reframeButton = window.effects()->findChild<QPushButton*>("auto-reframe");
    if (reframeButton == nullptr || !reframeButton->isEnabled()) {
        zaro::app::testing::failf("there is no usable auto-reframe button\n");
    }

    auto framed = window.reframeClip();
    if (!framed) {
        zaro::app::testing::failf("%s\n", framed.error().toString().c_str());
    }
    const auto* recomposed =
        window.project().findSequence(tallId)->findTrack(tallTrack)->find(tallClip);
    std::printf("  auto-reframe: %d frames, scaled to %.0f%%\n", framed->measured,
                framed->scale * 100.0);

    // Scaled to cover: 320x240 into a 240x320 frame needs 320/240.
    if (std::fabs(recomposed->transform.scaleX - (320.0 / 240.0)) > 0.01) {
        zaro::app::testing::failf("the scale does not fill the frame (%.3f)\n",
                                  recomposed->transform.scaleX);
    }
    if (recomposed->animation.find(zaro::model::Param::PositionX) == nullptr) {
        zaro::app::testing::failf("reframing wrote no position keyframes\n");
    }
    // And the frame is full: with the picture scaled to cover, no pixel
    // of the output is left transparent.
    window.setPosition(zaro::time::RationalTime{4, zaro::time::rates::fps25});
    window.renderCache().clear();
    zaro::render::RenderGraph graph{window.frameSource()};
    graph.setProject(&window.project());
    zaro::render::RgbaImage out;
    if (Status drawn =
            graph.compositeInto(*window.project().findSequence(tallId),
                                zaro::time::RationalTime{4, zaro::time::rates::fps25}, out);
        !drawn) {
        zaro::app::testing::failf("%s\n", drawn.error().toString().c_str());
    }
    int empty = 0;
    for (std::int32_t downTheFrame = 0; downTheFrame < out.height(); ++downTheFrame) {
        for (std::int32_t acrossIt = 0; acrossIt < out.width(); ++acrossIt) {
            empty += out.at(acrossIt, downTheFrame).a < 0.5F ? 1 : 0;
        }
    }
    if (empty > 0) {
        zaro::app::testing::failf("%d pixels of the reframed picture are empty\n", empty);
    }

    // Refused where somebody has already composed the shot by hand.
    auto again = zaro::edit::makeReframe(window.project(), {tallId, tallTrack}, tallClip,
                                         zaro::model::Curve{}, zaro::model::Curve{}, 1.0);
    if (again) {
        zaro::app::testing::failf("reframing over an animated clip was allowed\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.setActiveSequence(originalSequenceId);
    window.renderCache().clear();
    QApplication::processEvents();
}
