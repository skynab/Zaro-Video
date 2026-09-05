#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontMetrics>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QStyleOptionSpinBox>
// Titles, captions and the things pinned to a shot.
//
// Driven through the real window against the real compositor. See GuiFixture.h
// for what is shared and why.

#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "../FrameGrab.h"
#include "../TitleOverlay.h"
#include "../commands/Structure.h"
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

// Motion graphics templates: save a title, drop it in somewhere else.
TEST_CASE("A graphic template, saved and placed again", "[gui]") {
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

    const auto tplSequenceId = window.project().activeSequence();
    const auto* tplSequence = window.project().findSequence(tplSequenceId);
    const auto tplRate = tplSequence->frameRate();
    const auto tplTrackId = tplSequence->videoTracks().back().id();
    constexpr int kDesigned = 48;

    zaro::model::Graphic lower;
    lower.kind = zaro::model::GraphicKind::Text;
    lower.text = "TEMPLATE";
    lower.pointSize = 40.0;
    lower.bold = true;
    lower.width = 280.0;
    lower.height = 90.0;
    lower.red = 1.0;
    lower.green = 1.0;
    lower.blue = 1.0;
    auto made = zaro::edit::makeAddGraphic(
        window.project(), {tplSequenceId, tplTrackId}, lower,
        zaro::time::TimeRange{zaro::time::RationalTime{0, tplRate},
                              zaro::time::RationalTime{kDesigned, tplRate}});
    if (!made) {
        zaro::app::testing::failf("%s\n", made.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*made));
    zaro::model::ClipId designedId;
    for (const auto& candidate :
         window.project().findSequence(tplSequenceId)->findTrack(tplTrackId)->clips()) {
        if (candidate.graphic.text == "TEMPLATE") {
            designedId = candidate.id;
        }
    }
    if (!designedId.isValid()) {
        zaro::app::testing::failf("the template source was not added\n");
    }

    // Designed with a fade on and off, and both ends protected.
    zaro::model::Curve fade;
    for (const auto& [frame, value] :
         {std::pair{0, 0.0}, std::pair{12, 1.0}, std::pair{36, 1.0}, std::pair{kDesigned, 0.0}}) {
        fade.set(zaro::model::Keyframe{zaro::time::RationalTime{frame, tplRate},
                                       value,
                                       zaro::model::Interpolation::Linear,
                                       {},
                                       {}});
    }
    auto animated = zaro::edit::makeSetCurve(window.project(), {tplSequenceId, tplTrackId},
                                             designedId, zaro::model::Param::Opacity, fade);
    if (!animated) {
        zaro::app::testing::failf("%s\n", animated.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*animated));
    auto responsive = zaro::edit::makeSetResponsive(
        window.project(), {tplSequenceId, tplTrackId}, designedId,
        zaro::time::RationalTime{12, tplRate}, zaro::time::RationalTime{12, tplRate});
    if (!responsive) {
        zaro::app::testing::failf("%s\n", responsive.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*responsive));

    timeline->selectOnly(tplTrackId, designedId);
    window.effects()->setSelection(tplTrackId, designedId);
    QApplication::processEvents();

    const std::string templatePath =
        (std::filesystem::temp_directory_path() / "zaro-selftest-lower-third.zarograph").string();
    std::filesystem::remove(templatePath);
    if (Status saved = window.saveGraphicTemplate(templatePath); !saved) {
        zaro::app::testing::failf("%s\n", saved.error().toString().c_str());
    }

    // Dropped in later on the timeline, at half the length it was
    // designed at.
    const auto dropAt = zaro::time::RationalTime{120, tplRate};
    window.setPosition(dropAt);
    QApplication::processEvents();
    auto dropped = window.placeGraphicTemplate(templatePath, zaro::time::RationalTime{24, tplRate});
    if (!dropped) {
        zaro::app::testing::failf("%s\n", dropped.error().toString().c_str());
    }
    const auto* copy =
        window.project().findSequence(tplSequenceId)->findTrack(tplTrackId)->find(*dropped);
    if (copy == nullptr || copy->graphic.text != "TEMPLATE" || copy->id == designedId) {
        zaro::app::testing::failf("the template did not arrive as its own clip\n");
    }
    if (copy->timelineRange.duration().frames() != 24 ||
        copy->responsive.authored.frames() != kDesigned) {
        zaro::app::testing::failf(
            "the placed template took the wrong length or lost its "
            "responsive timing\n");
    }

    // The animation came with it and still runs at the speed it was
    // drawn at: dark on the first frame, up twelve frames in, dark
    // again on the last.
    const double onEntry = copy->transformAt(dropAt).opacity;
    const double held = copy->transformAt(dropAt + zaro::time::RationalTime{12, tplRate}).opacity;
    const double onExit = copy->transformAt(dropAt + zaro::time::RationalTime{24, tplRate}).opacity;
    std::printf("  graphic template: opacity %.2f in, %.2f held, %.2f out\n", onEntry, held,
                onExit);
    if (!(onEntry < 0.1 && held > 0.9 && onExit < 0.1)) {
        zaro::app::testing::failf("the placed template does not animate\n");
    }

    // And it reaches the picture, not just the model.
    window.setPosition(dropAt + zaro::time::RationalTime{12, tplRate});
    window.renderCache().clear();
    const double litTemplate = meanGray(settledGrab(window.monitor()));
    window.setPosition(dropAt);
    window.renderCache().clear();
    const double darkTemplate = meanGray(settledGrab(window.monitor()));
    if (!(litTemplate > darkTemplate)) {
        zaro::app::testing::failf("the placed template is not on screen (%.1f vs %.1f)\n",
                                  litTemplate, darkTemplate);
    }

    std::filesystem::remove(templatePath);
    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.renderCache().clear();
    window.monitor()->update();
    QApplication::processEvents();
}

// Captions: imported from a real file, burned in, and measured through
// the compositor. The parser and the burn-in are tested headlessly;
// what those cannot show is whether an imported file reaches the
// picture.
TEST_CASE("Captions imported, burned in and measured", "[gui]") {
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

    // The file is written here rather than found: this block used to
    // read a .srt from the scratch folder of the session that wrote it,
    // which is a check that stops checking the moment that folder goes.
    const std::filesystem::path subtitlePath =
        std::filesystem::temp_directory_path() / "zaro-selftest-captions.srt";
    {
        std::ofstream writing{subtitlePath};
        // Back to back and covering the whole two seconds the check
        // searches: a gap between them would leave the darkest frame
        // uncaptioned, and the test would read that as the burn-in not
        // working.
        writing << "1\n00:00:00,000 --> 00:00:01,000\nfirst line\n\n"
                << "2\n00:00:01,000 --> 00:00:02,000\nsecond line\n\n";
    }
    auto subtitles = zaro::io::loadSubtitles(subtitlePath.string());
    std::filesystem::remove(subtitlePath);
    if (!subtitles) {
        zaro::app::testing::failf("%s\n", subtitles.error().toString().c_str());
    }
    if (subtitles->size() != 2) {
        zaro::app::testing::failf("read %zu captions, expected 2\n", subtitles->size());
    }

    std::int64_t darkFrame = 0;
    double darkest = 1e9;
    for (std::int64_t frame = 0; frame < 40; ++frame) {
        window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
        const double gray = meanGray(settledGrab(window.monitor()));
        if (gray < darkest) {
            darkest = gray;
            darkFrame = frame;
        }
    }

    zaro::model::CaptionTrack burned = *subtitles;
    burned.setBurnedIn(true);
    auto built = zaro::edit::makeSetCaptions(window.project(), sequence.id(), burned);
    if (!built) {
        zaro::app::testing::failf("%s\n", built.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*built));

    // The darkest frame inside the caption's span, not frame zero: this
    // fixture's frame zero is a white flash, and a white caption on a
    // white frame is invisible. The caption runs to two seconds, which
    // covers the whole range searched above.
    window.setPosition(zaro::time::RationalTime{darkFrame, sequence.frameRate()});
    window.monitor()->update();
    QApplication::processEvents();
    const double withCaption = meanGray(settledGrab(window.monitor()));

    window.commands().undo(window.project());
    window.monitor()->update();
    QApplication::processEvents();
    const double without = meanGray(settledGrab(window.monitor()));

    std::printf("  captions: %zu read, frame reads %.2f burned in against %.2f\n",
                subtitles->size(), withCaption, without);
    if (!(withCaption > without + 0.2)) {
        zaro::app::testing::failf("the burned-in caption did not reach the picture\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.monitor()->update();
    QApplication::processEvents();
}

// A text layer, through the real font engine and the real compositor.
// The coverage-to-colour step is tested headlessly against a stand-in
// engine; what that cannot show is whether Qt is actually being asked
// for glyphs and whether they reach the screen.
TEST_CASE("A text layer, through the real font engine", "[gui]") {
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
    const double blank = meanGray(settledGrab(window.monitor()));

    zaro::model::Graphic title;
    title.kind = zaro::model::GraphicKind::Text;
    title.text = "ZARO";
    title.pointSize = 160.0;
    title.bold = true;
    title.width = 2000.0;
    title.height = 800.0;
    title.red = 1.0;
    title.green = 1.0;
    title.blue = 1.0;

    auto built = zaro::edit::makeAddGraphic(
        window.project(), {sequence.id(), videoTrack.id()}, title,
        zaro::time::TimeRange{zaro::time::RationalTime{0, sequence.frameRate()},
                              zaro::time::RationalTime{40, sequence.frameRate()}});
    if (!built) {
        zaro::app::testing::failf("%s\n", built.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*built));
    window.monitor()->update();
    QApplication::processEvents();
    const double withText = meanGray(settledGrab(window.monitor()));

    // Glyphs cover a small fraction of the frame, so this is not a big
    // number -- but it is unambiguously more than nothing, and nothing
    // is what a missing font engine produces.
    std::printf("  text layer: %.2f with a title, %.2f without\n", withText, blank);
    if (!(withText > blank + 0.5)) {
        zaro::app::testing::failf(
            "the text layer drew nothing; either Qt was not asked for "
            "glyphs or they did not reach the compositor\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.monitor()->update();
    QApplication::processEvents();
}

// Making a title, the way somebody makes one.
//
// The model, the font engine and the compositor for text have all been in the
// tree since the graphics work; what was missing was any way to ask for a
// title. The bin's own Titles tab said "add a title clip to a video track" and
// nothing anywhere added one. This covers the action behind that sentence: it
// makes a clip that says something, at the playhead, on a picture row, without
// overwriting what is already cut there -- and one undo takes it back.
TEST_CASE("Adding a title from the action", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    const auto& sequence = *window.sequence();
    const auto v1 = sequence.videoTracks().front().id();

    // Parked over the fixture's own clip, so the row under the playhead is
    // busy: a title dropped on top of it would overwrite the cut.
    window.setPosition(zaro::time::RationalTime{10, sequence.frameRate()});
    QApplication::processEvents();
    const std::size_t videoTracksBefore = window.sequence()->videoTracks().size();
    const std::size_t v1ClipsBefore = window.sequence()->findTrack(v1)->clips().size();
    const std::size_t stepsBefore = window.commands().position();
    const double blank = meanGray(settledGrab(window.monitor()));

    window.addTitle();
    QApplication::processEvents();

    if (window.sequence()->videoTracks().size() != videoTracksBefore + 1) {
        zaro::app::testing::failf("the title did not take a picture row of its own\n");
    }
    if (window.sequence()->findTrack(v1)->clips().size() != v1ClipsBefore) {
        zaro::app::testing::failf("the title overwrote what was already on V1\n");
    }
    const zaro::model::Track& made = window.sequence()->videoTracks().back();
    if (made.clips().size() != 1) {
        zaro::app::testing::failf("the new row holds %zu clips, not one\n", made.clips().size());
    }
    const zaro::model::Clip& title = made.clips().front();
    if (title.graphic.kind != zaro::model::GraphicKind::Text) {
        zaro::app::testing::failf("what was added is not a text layer\n");
    }
    if (title.graphic.text.empty()) {
        zaro::app::testing::failf("the title says nothing at all\n");
    }
    if (title.start() != zaro::time::RationalTime{10, sequence.frameRate()}) {
        zaro::app::testing::failf("the title starts at %lld, not at the playhead\n",
                                  static_cast<long long>(title.start().frames()));
    }
    // Sized to this frame rather than to a fixed number of pixels: the
    // rasteriser reads `pointSize` as pixels, so a default that ignored the
    // sequence would be illegible on one cut and enormous on another.
    if (!(title.graphic.pointSize > 0.0) ||
        title.graphic.width > static_cast<double>(sequence.width())) {
        zaro::app::testing::failf("the title's box does not fit the frame\n");
    }
    // Selected, because the next thing anybody does is type over it.
    if (window.editContext().clip != title.id) {
        zaro::app::testing::failf("the new title is not the selection\n");
    }

    // And it reaches the picture.
    //
    // A small number on purpose: the fixture is 320x240 and the default title
    // is sized to the frame, so five glyphs at twenty pixels cover a fraction
    // of a percent of it. Against a frame that reads 0.00 it is still
    // unambiguous, and what it guards against is nothing at all -- a title that
    // never reached the compositor, or a font engine never asked for glyphs.
    const double withTitle = meanGray(settledGrab(window.monitor()));
    std::printf("  title: %.2f with it, %.2f without, %s at %lld\n", withTitle, blank,
                title.graphic.text.c_str(), static_cast<long long>(title.start().frames()));
    if (!(withTitle > blank + 0.05)) {
        zaro::app::testing::failf("the title did not reach the monitor (%.2f against %.2f)\n",
                                  withTitle, blank);
    }

    // One undo step, including the row it had to make.
    if (window.commands().position() != stepsBefore + 1) {
        zaro::app::testing::failf("making a title took %zu undo steps, not one\n",
                                  window.commands().position() - stepsBefore);
    }
    window.commands().undo(window.project());
    if (window.sequence()->videoTracks().size() != videoTracksBefore) {
        zaro::app::testing::failf("undo left the row the title made behind\n");
    }
    window.monitor()->update();
    QApplication::processEvents();
}

// The same thing again, from the button on the timeline rather than from the
// action behind it.
//
// The action was reachable by keystroke, from the right-click menu and from the
// bin's Titles tab, and from nothing visible on the timeline -- so the answer to
// "how do I put text on this" was three places none of which were where somebody
// was looking. This is that button: it exists, and pressing it does what the
// action does.
TEST_CASE("Adding a title from the timeline button", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    const auto& sequence = *window.sequence();

    auto* button = window.findChild<QPushButton*>("timeline-add-title");
    if (button == nullptr) {
        zaro::app::testing::failf("there is no Add Title button on the timeline\n");
        return;
    }

    window.setPosition(zaro::time::RationalTime{10, sequence.frameRate()});
    QApplication::processEvents();
    const std::size_t videoTracksBefore = window.sequence()->videoTracks().size();

    button->click();
    QApplication::processEvents();

    if (window.sequence()->videoTracks().size() != videoTracksBefore + 1) {
        zaro::app::testing::failf("the button did not put a title on a row of its own\n");
        return;
    }
    const zaro::model::Track& made = window.sequence()->videoTracks().back();
    if (made.clips().size() != 1 ||
        made.clips().front().graphic.kind != zaro::model::GraphicKind::Text) {
        zaro::app::testing::failf("the button made something that is not a text layer\n");
        return;
    }
    if (made.clips().front().start() != zaro::time::RationalTime{10, sequence.frameRate()}) {
        zaro::app::testing::failf("the title did not land at the playhead\n");
    }
    window.commands().undo(window.project());
    window.monitor()->update();
    QApplication::processEvents();
}

// Pinning a title to the shot under it, through the real panel.
TEST_CASE("Pinning a title to the shot under it", "[gui]") {
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

    const auto pinSequenceId = window.project().activeSequence();
    if (window.project().findSequence(pinSequenceId)->videoTracks().size() < 2) {
        auto added = zaro::edit::makeAddTrack(window.project(), pinSequenceId,
                                              zaro::model::TrackKind::Video, "V2");
        if (!added) {
            zaro::app::testing::failf("%s\n", added.error().toString().c_str());
        }
        window.commands().execute(window.project(), std::move(*added));
    }
    const auto* pinSequence = window.project().findSequence(pinSequenceId);
    const auto pinRate = pinSequence->frameRate();
    const auto lowerTrack = pinSequence->videoTracks().front().id();
    const auto upperTrack = pinSequence->videoTracks().back().id();
    const auto pinAt = zaro::time::RationalTime{6, pinRate};
    const auto* under = pinSequence->findTrack(lowerTrack)->clipAt(pinAt);
    if (under == nullptr) {
        zaro::app::testing::failf("there is no shot to pin to\n");
    }
    const auto hostId = under->id;

    zaro::model::Graphic badge;
    badge.kind = zaro::model::GraphicKind::Rectangle;
    badge.width = 60.0;
    badge.height = 40.0;
    badge.red = 1.0;
    badge.green = 1.0;
    badge.blue = 1.0;
    badge.centreX = 0.0;
    auto placed =
        zaro::edit::makeAddGraphic(window.project(), {pinSequenceId, upperTrack}, badge,
                                   zaro::time::TimeRange{zaro::time::RationalTime{0, pinRate},
                                                         zaro::time::RationalTime{20, pinRate}});
    if (!placed) {
        zaro::app::testing::failf("%s\n", placed.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*placed));
    zaro::model::ClipId badgeId;
    for (const auto& candidate :
         window.project().findSequence(pinSequenceId)->findTrack(upperTrack)->clips()) {
        if (candidate.graphic.width == 60.0) {
            badgeId = candidate.id;
        }
    }
    if (!badgeId.isValid()) {
        zaro::app::testing::failf("the badge was not added\n");
    }
    // Offset from the middle, so following a scale is visible as a
    // move rather than only as a size.
    auto offset =
        zaro::edit::makeSetTransform(window.project(), {pinSequenceId, upperTrack}, badgeId, [] {
            zaro::model::Transform transform;
            transform.positionX = 60.0;
            return transform;
        }());
    if (!offset) {
        zaro::app::testing::failf("%s\n", offset.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*offset));

    timeline->selectOnly(upperTrack, badgeId);
    window.effects()->setSelection(upperTrack, badgeId);
    window.setPosition(pinAt);
    QApplication::processEvents();
    auto* pinButton = window.effects()->findChild<QPushButton*>("pin-to-clip");
    auto* unpinButton = window.effects()->findChild<QPushButton*>("unpin");
    if (pinButton == nullptr || unpinButton == nullptr || !pinButton->isEnabled()) {
        zaro::app::testing::failf("the pin controls are not in the panel\n");
    }
    if (unpinButton->isEnabled()) {
        zaro::app::testing::failf("an unpinned clip offers to unpin\n");
    }
    pinButton->click();
    QApplication::processEvents();
    const auto* pinnedBadge =
        window.project().findSequence(pinSequenceId)->findTrack(upperTrack)->find(badgeId);
    if (pinnedBadge->pinnedTo != hostId) {
        zaro::app::testing::failf("the button pinned it to the wrong thing\n");
    }

    // Now move and scale the shot, and the badge has to come with it.
    const auto badgeBefore = zaro::model::pinnedTransformAt(
        *window.project().findSequence(pinSequenceId), *pinnedBadge, pinAt);
    auto shifted =
        zaro::edit::makeSetTransform(window.project(), {pinSequenceId, lowerTrack}, hostId, [] {
            zaro::model::Transform transform;
            transform.positionX = 40.0;
            transform.positionY = -30.0;
            transform.scaleX = 1.5;
            transform.scaleY = 1.5;
            return transform;
        }());
    if (!shifted) {
        zaro::app::testing::failf("%s\n", shifted.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*shifted));
    const zaro::model::Sequence* movedSequence = window.project().findSequence(pinSequenceId);
    const auto badgeAfter = zaro::model::pinnedTransformAt(
        *movedSequence, *movedSequence->findTrack(upperTrack)->find(badgeId), pinAt);
    std::printf("  pin to clip: badge at %.0f,%.0f scale %.2f -> %.0f,%.0f scale %.2f\n",
                badgeBefore.positionX, badgeBefore.positionY, badgeBefore.scaleX,
                badgeAfter.positionX, badgeAfter.positionY, badgeAfter.scaleX);
    if (std::fabs(badgeAfter.positionX - (40.0 + (60.0 * 1.5))) > 0.01 ||
        std::fabs(badgeAfter.positionY + 30.0) > 0.01 ||
        std::fabs(badgeAfter.scaleX - 1.5) > 0.01) {
        zaro::app::testing::failf("the pinned badge did not follow the shot\n");
    }

    // The cache has to know where the host is, even when nothing else
    // in the frame mentions it. With the host's own track hidden, the
    // recipe records only that the track is hidden -- so without the
    // pin being mixed in, moving the host would leave the pinned clip
    // cached where it was.
    window.project().findSequence(pinSequenceId)->findTrack(lowerTrack)->setMuted(true);
    movedSequence = window.project().findSequence(pinSequenceId);
    const std::uint64_t recipeAfter =
        zaro::render::frameRecipe(&window.project(), *movedSequence, pinAt);
    auto unshifted = zaro::edit::makeSetTransform(window.project(), {pinSequenceId, lowerTrack},
                                                  hostId, zaro::model::Transform{});
    if (!unshifted) {
        zaro::app::testing::failf("%s\n", unshifted.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*unshifted));
    const std::uint64_t recipeBack = zaro::render::frameRecipe(
        &window.project(), *window.project().findSequence(pinSequenceId), pinAt);
    window.project().findSequence(pinSequenceId)->findTrack(lowerTrack)->setMuted(false);
    if (recipeAfter == recipeBack) {
        zaro::app::testing::failf("moving the host did not change the pinned clip's recipe\n");
    }

    window.effects()->setSelection(upperTrack, badgeId);
    QApplication::processEvents();
    if (!unpinButton->isEnabled()) {
        zaro::app::testing::failf("a pinned clip offers no way to unpin\n");
    }
    unpinButton->click();
    QApplication::processEvents();
    if (window.project()
            .findSequence(pinSequenceId)
            ->findTrack(upperTrack)
            ->find(badgeId)
            ->pinnedTo.isValid()) {
        zaro::app::testing::failf("unpinning did not take\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.renderCache().clear();
    window.monitor()->update();
    QApplication::processEvents();
}
// Placing a title on the picture, by dragging it.
//
// `centreX` and `width` are spin boxes in the inspector, and nobody composes a
// frame by typing numbers into it. The overlay edits the same two properties
// where the answer is visible. What this covers is that the gesture reaches the
// model in the right units and the right direction, that a whole drag is one
// undo step, and that the box is only up for a clip that is a title.
TEST_CASE("A title is placed by dragging it on the picture", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    const auto& sequence = *window.sequence();

    window.setPosition(zaro::time::RationalTime{10, sequence.frameRate()});
    QApplication::processEvents();
    window.addTitle();
    QApplication::processEvents();

    auto* overlay = window.findChild<zaro::app::TitleOverlay*>();
    if (overlay == nullptr) {
        zaro::app::testing::failf("the window has no title overlay\n");
    }
    if (!overlay->isEditing()) {
        zaro::app::testing::failf("the overlay is not up for the title just made\n");
    }

    const auto titleNow = [&window]() -> const zaro::model::Clip* {
        for (const zaro::model::Track& track : window.sequence()->videoTracks()) {
            for (const zaro::model::Clip& clip : track.clips()) {
                if (clip.graphic.kind == zaro::model::GraphicKind::Text) {
                    return &clip;
                }
            }
        }
        return nullptr;
    };
    const zaro::model::Clip* title = titleNow();
    REQUIRE(title != nullptr);
    const zaro::model::Graphic before = title->graphic;

    // Alt held throughout: snapping is what the gesture is for, but a test that
    // let an edge latch would be measuring the snap rather than the drag.
    const QPointF centre = window.monitor()->pictureRect().center();
    const auto drag = [overlay](const QPointF& from, const QPointF& to) {
        const auto send = [overlay](QEvent::Type type, const QPointF& at, Qt::MouseButton button,
                                    Qt::MouseButtons buttons) {
            QMouseEvent event{type, at, at, button, buttons, Qt::AltModifier};
            QCoreApplication::sendEvent(overlay, &event);
        };
        send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        for (int step = 1; step <= 6; ++step) {
            const QPointF at{from.x() + (to.x() - from.x()) * step / 6.0,
                             from.y() + (to.y() - from.y()) * step / 6.0};
            send(QEvent::MouseMove, at, Qt::NoButton, Qt::LeftButton);
        }
        send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
        QApplication::processEvents();
    };

    // How many pixels of frame one pixel of widget is worth, which is what the
    // drag has to be measured in: the monitor letterboxes the picture, so the
    // two are not the same number.
    const double perPixel = static_cast<double>(sequence.width()) /
                            std::max(1.0, window.monitor()->pictureRect().width());

    const std::size_t stepsBefore = window.commands().position();
    // Grab the box where it actually is rather than in the middle of the
    // frame. A Title sits across the top, so a press on the picture's centre
    // lands on nothing and starts no drag -- which would make this a test of
    // where the preset happens to put the box rather than of the gesture.
    const QPointF grab{centre.x() + before.centreX / perPixel,
                       centre.y() + before.centreY / perPixel};
    drag(grab, QPointF{grab.x() + 40.0, grab.y() + 20.0});

    title = titleNow();
    REQUIRE(title != nullptr);
    const double movedX = title->graphic.centreX - before.centreX;
    const double movedY = title->graphic.centreY - before.centreY;
    if (!(movedX > 0.0) || !(movedY > 0.0)) {
        zaro::app::testing::failf("the box did not follow the pointer (%.1f, %.1f)\n", movedX,
                                  movedY);
    }
    // Within a pixel of frame: the drag is in widget pixels and the model is in
    // output pixels, and getting the conversion wrong is the bug this catches.
    if (std::abs(movedX - 40.0 * perPixel) > perPixel ||
        std::abs(movedY - 20.0 * perPixel) > perPixel) {
        zaro::app::testing::failf("the box moved %.1f,%.1f, not %.1f,%.1f\n", movedX, movedY,
                                  40.0 * perPixel, 20.0 * perPixel);
    }
    // The box moved and nothing else did.
    if (title->graphic.width != before.width || title->graphic.height != before.height) {
        zaro::app::testing::failf("moving the box resized it\n");
    }
    if (window.commands().position() != stepsBefore + 1) {
        zaro::app::testing::failf("one drag made %zu undo steps, not one\n",
                                  window.commands().position() - stepsBefore);
    }

    // A corner sizes the box and leaves the opposite corner where it was.
    const zaro::model::Graphic moved = title->graphic;
    const QPointF picture = window.monitor()->pictureRect().center();
    const QPointF corner{picture.x() + (moved.centreX + moved.width * 0.5) / perPixel,
                         picture.y() + (moved.centreY + moved.height * 0.5) / perPixel};
    const double leftEdge = moved.centreX - moved.width * 0.5;
    drag(corner, QPointF{corner.x() - 30.0, corner.y()});

    title = titleNow();
    REQUIRE(title != nullptr);
    if (!(title->graphic.width < moved.width)) {
        zaro::app::testing::failf("dragging the corner in did not narrow the box (%.1f to %.1f)\n",
                                  moved.width, title->graphic.width);
    }
    if (std::abs((title->graphic.centreX - title->graphic.width * 0.5) - leftEdge) > 1.0) {
        zaro::app::testing::failf("the far edge moved while a corner was dragged\n");
    }
    if (std::abs(title->graphic.height - moved.height) > 0.001) {
        zaro::app::testing::failf("a horizontal drag changed the height\n");
    }

    std::printf("  title box: moved %.0f,%.0f px of frame, narrowed to %.0f\n", movedX, movedY,
                title->graphic.width);

    // And the box belongs to titles alone: pick the shot underneath and it goes
    // away rather than sitting over a clip it cannot edit.
    const auto& pictureTrack = window.sequence()->videoTracks().front();
    if (!pictureTrack.clips().empty()) {
        window.timeline()->selectOnly(pictureTrack.id(), pictureTrack.clips().front().id);
        QApplication::processEvents();
        if (overlay->isEditing() || overlay->isVisible()) {
            zaro::app::testing::failf("the title box stayed up over a clip that is not a title\n");
        }
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    QApplication::processEvents();
}

// Typing the text on the picture, which is where somebody is looking at it.
//
// The text was editable in one place: a field in the inspector, on the far side
// of the window from the thing it changes. This is a double-click on the title
// itself -- the editor opens over the box, in roughly the face it will be drawn
// in, and what is typed reaches the model as it is typed. Escape puts back what
// was there, and a whole typing pass is one undo step rather than one per
// letter.
TEST_CASE("A title's text is typed on the picture", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    const auto& sequence = *window.sequence();

    window.setPosition(zaro::time::RationalTime{10, sequence.frameRate()});
    QApplication::processEvents();
    window.addTitle();
    QApplication::processEvents();

    auto* overlay = window.findChild<zaro::app::TitleOverlay*>();
    REQUIRE(overlay != nullptr);
    REQUIRE(overlay->isEditing());

    const auto titleNow = [&window]() -> const zaro::model::Clip* {
        for (const zaro::model::Track& track : window.sequence()->videoTracks()) {
            for (const zaro::model::Clip& clip : track.clips()) {
                if (clip.graphic.kind == zaro::model::GraphicKind::Text) {
                    return &clip;
                }
            }
        }
        return nullptr;
    };
    REQUIRE(titleNow() != nullptr);
    const std::string before = titleNow()->graphic.text;

    const QPointF centre = window.monitor()->pictureRect().center();
    // `wobble` is how far the hand drifts between the two clicks, in pixels.
    // Zero is a machine; a few pixels is a person.
    const auto doubleClick = [overlay](const QPointF& at, double wobble = 0.0) {
        const auto send = [overlay](QEvent::Type type, const QPointF& where) {
            QMouseEvent event{type, where, where, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier};
            QCoreApplication::sendEvent(overlay, &event);
        };
        send(QEvent::MouseButtonPress, at);
        if (wobble > 0.0) {
            const QPointF drifted{at.x() + wobble, at.y() + wobble};
            QMouseEvent moved{QEvent::MouseMove, drifted,        drifted,
                              Qt::NoButton,      Qt::LeftButton, Qt::NoModifier};
            QCoreApplication::sendEvent(overlay, &moved);
        }
        send(QEvent::MouseButtonRelease, at);
        send(QEvent::MouseButtonDblClick, at);
        QApplication::processEvents();
    };

    if (overlay->isTyping()) {
        zaro::app::testing::failf("the overlay was already open for typing\n");
    }
    // Dragged off the guides first, with Alt so it does not latch onto one.
    // A title arrives sitting on the centre line horizontally, and a box
    // already on a guide is the one case where snapping it again moves nothing
    // -- so a test that double-clicked a fresh title would pass whatever the
    // press did.
    //
    // The drag starts from the box rather than from the middle of the picture:
    // a Title sits across the top, and pressing where it is not does nothing.
    const double perPixel = static_cast<double>(sequence.width()) /
                            std::max(1.0, window.monitor()->pictureRect().width());
    const QPointF start{centre.x() + titleNow()->graphic.centreX / perPixel,
                        centre.y() + titleNow()->graphic.centreY / perPixel};
    {
        const auto send = [overlay](QEvent::Type type, const QPointF& where, Qt::MouseButton button,
                                    Qt::MouseButtons buttons) {
            QMouseEvent event{type, where, where, button, buttons, Qt::AltModifier};
            QCoreApplication::sendEvent(overlay, &event);
        };
        send(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        for (int step = 1; step <= 4; ++step) {
            const QPointF at{start.x() + 9.0 * step, start.y() + 7.0 * step};
            send(QEvent::MouseMove, at, Qt::NoButton, Qt::LeftButton);
        }
        send(QEvent::MouseButtonRelease, QPointF{start.x() + 36.0, start.y() + 28.0},
             Qt::LeftButton, Qt::NoButton);
        QApplication::processEvents();
    }
    const QPointF box = start + QPointF{36.0, 28.0};

    // Where the box is before the double-click, and a hand that is not
    // perfectly still between the two clicks. Opening a title to type in it
    // must not move it: the press used to start a drag straight away, so the
    // tremor was a move and snapping pulled the box onto the nearest guide.
    const zaro::model::Graphic placed = titleNow()->graphic;
    if (placed.centreX == 0.0 && placed.centreY == 0.0) {
        zaro::app::testing::failf("the title did not move off the guides to begin with\n");
    }
    const std::size_t stepsAtOpen = window.commands().position();
    doubleClick(box, 6.0);
    if (!overlay->isTyping()) {
        zaro::app::testing::failf("a double-click on the title did not open it for typing\n");
        return;
    }
    if (titleNow()->graphic.centreX != placed.centreX ||
        titleNow()->graphic.centreY != placed.centreY) {
        zaro::app::testing::failf("the double-click moved the title to %.1f,%.1f from %.1f,%.1f\n",
                                  titleNow()->graphic.centreX, titleNow()->graphic.centreY,
                                  placed.centreX, placed.centreY);
    }
    if (window.commands().position() != stepsAtOpen) {
        zaro::app::testing::failf("opening the editor put a step on the history\n");
    }

    auto* editor = overlay->findChild<QPlainTextEdit*>();
    REQUIRE(editor != nullptr);
    // Selected on open, so typing replaces the placeholder rather than landing
    // beside it -- a title arrives saying "Title" and nobody wants that kept.
    if (!editor->textCursor().hasSelection()) {
        zaro::app::testing::failf("the text was not selected when the editor opened\n");
    }

    const std::size_t stepsBefore = window.commands().position();
    editor->setPlainText("Kestrel Bay");
    QApplication::processEvents();
    if (titleNow()->graphic.text != "Kestrel Bay") {
        zaro::app::testing::failf("typing did not reach the model: the title says \"%s\"\n",
                                  titleNow()->graphic.text.c_str());
    }

    // Return finishes. Shift+Return would be a line break, which is why the
    // plain one is what closes.
    QKeyEvent done{QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QCoreApplication::sendEvent(editor, &done);
    QApplication::processEvents();
    if (overlay->isTyping()) {
        zaro::app::testing::failf("Return did not finish typing\n");
    }
    if (titleNow()->graphic.text != "Kestrel Bay") {
        zaro::app::testing::failf("finishing threw away what was typed\n");
    }
    if (window.commands().position() - stepsBefore != 1) {
        zaro::app::testing::failf("typing took %zu undo steps, not one\n",
                                  window.commands().position() - stepsBefore);
    }

    // Escape puts back whatever the text was when the pass started.
    doubleClick(box);
    REQUIRE(overlay->isTyping());
    editor->setPlainText("thrown away");
    QApplication::processEvents();
    const std::size_t stepsBeforeEscape = window.commands().position();
    QKeyEvent give{QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier};
    QCoreApplication::sendEvent(editor, &give);
    QApplication::processEvents();
    // Abandoned, so nothing is left on the history: a step that changes nothing
    // is a Ctrl+Z that appears to do nothing.
    if (window.commands().position() >= stepsBeforeEscape) {
        zaro::app::testing::failf("Escape left a step on the history\n");
    }
    if (overlay->isTyping()) {
        zaro::app::testing::failf("Escape did not close the editor\n");
    }
    if (titleNow()->graphic.text != "Kestrel Bay") {
        zaro::app::testing::failf("Escape left \"%s\" behind instead of putting the text back\n",
                                  titleNow()->graphic.text.c_str());
    }

    // And one undo takes the whole typing pass back to what the title said when
    // it was made.
    window.commands().undo(window.project());
    if (titleNow()->graphic.text != before) {
        zaro::app::testing::failf("undo left \"%s\", not the text the title arrived with\n",
                                  titleNow()->graphic.text.c_str());
    }
    window.monitor()->update();
    QApplication::processEvents();
}

// The controls the title panel is operated through, measured off what is
// actually painted.
//
// Both of these broke on the same thing: styling a subcontrol takes a widget
// off Qt's native painter, and Qt then draws no arrow unless the sheet names an
// image. Every combo lost its caret and every number field lost its steppers --
// and once the caret was drawn back, the zone it sits in was wider than the one
// Qt reserves when it decides how wide a combo wants to be, so the text under it
// was clipped.
//
// Measured from pixels, and only from pixels. Two easier tests were written
// first and both passed against the broken build: asking the style for a
// subcontrol's rectangle reports the same field width whether or not the sheet
// has taken 26px out of it, and asking a laid-out combo whether its text fits
// asks the wrong widget, since the panel is wide enough to hide the problem.
// What clipped was a combo at the width it asks for, so that is the width drawn
// here.
TEST_CASE("The title panel's controls have room for their chrome", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;

    window.addTitle();
    QApplication::processEvents();

    auto* align = window.effects()->findChild<QComboBox*>("text-align");
    auto* size = window.effects()->findChild<QDoubleSpinBox*>("text-size");
    REQUIRE(align != nullptr);
    REQUIRE(size != nullptr);

    /// Which columns of an image have something drawn in them, against the
    /// colour found at `ground`. Used to tell an arrow from an empty button.
    const auto inkedColumns = [](const QImage& image, const QPoint& ground, const QRect& within) {
        std::vector<int> columns;
        const QRgb base = image.pixel(ground);
        const QRect box = within.intersected(image.rect());
        for (int x = box.left(); x <= box.right(); ++x) {
            for (int y = box.top(); y <= box.bottom(); ++y) {
                const QRgb here = image.pixel(x, y);
                const int apart = std::abs(qRed(here) - qRed(base)) +
                                  std::abs(qGreen(here) - qGreen(base)) +
                                  std::abs(qBlue(here) - qBlue(base));
                if (apart > 24) {
                    columns.push_back(x);
                    break;
                }
            }
        }
        return columns;
    };

    // The combo has to reserve room for its own arrow.
    //
    // Not measured off pixels, though that was tried: the text does not clip in
    // this run's fallback face, and it clipped in Inter -- a test that renders
    // and looks passes on the machine that is not the one with the problem. The
    // fault is in the arithmetic behind the render, so the arithmetic is what is
    // checked. Qt decides how wide a combo wants to be from the *native* arrow,
    // while the sheet then spends 26px of that on a drop-down zone of its own;
    // when the difference came to a pixel the word fitted here and was cut off
    // there.
    //
    // 40px is that zone plus the padding and border either side of the text,
    // plus enough over to be a margin rather than a coincidence.
    {
        constexpr int kChromeAndMargin = 40;
        const QFontMetrics metrics{align->font()};
        for (int item = 0; item < align->count(); ++item) {
            auto* probe = new QComboBox(window.effects());
            probe->addItem(align->itemText(item));
            const int needs = metrics.horizontalAdvance(align->itemText(item));
            const int asks = probe->sizeHint().width();
            if (asks - needs < kChromeAndMargin) {
                zaro::app::testing::failf(
                    "\"%s\" measures %dpx and its combo asks for %dpx: %dpx for chrome that "
                    "needs %d\n",
                    align->itemText(item).toUtf8().constData(), needs, asks, asks - needs,
                    kChromeAndMargin);
            }
            probe->deleteLater();
        }
    }

    // The steppers, counted as ink against the field beside them -- not against
    // the image corner, which is outside the rounded border and transparent, so
    // every pixel of the field counted as an arrow.
    {
        QImage shot{size->size(), QImage::Format_ARGB32};
        shot.fill(Qt::transparent);
        size->render(&shot);
        QStyleOptionSpinBox spin;
        spin.initFrom(size);
        spin.rect = size->rect();
        const QRect up =
            size->style()->subControlRect(QStyle::CC_SpinBox, &spin, QStyle::SC_SpinBoxUp, size);
        const QRect down =
            size->style()->subControlRect(QStyle::CC_SpinBox, &spin, QStyle::SC_SpinBoxDown, size);
        if (up.isEmpty() || down.isEmpty()) {
            zaro::app::testing::failf("the size field has no stepper buttons at all\n");
            return;
        }
        const QPoint ground{std::max(0, up.left() - 6), shot.height() / 2};
        // Inset, so the field's own border and its rounded corners are not
        // mistaken for something drawn in the button.
        const auto arrowInk = [&](const QRect& button) {
            return static_cast<int>(
                inkedColumns(shot, ground, button.adjusted(2, 2, -3, -2)).size());
        };
        constexpr int kLeastColumns = 4;
        if (arrowInk(up) < kLeastColumns) {
            zaro::app::testing::failf("nothing is drawn in the up stepper (%d columns of ink)\n",
                                      arrowInk(up));
        }
        if (arrowInk(down) < kLeastColumns) {
            zaro::app::testing::failf("nothing is drawn in the down stepper (%d columns of ink)\n",
                                      arrowInk(down));
        }
    }

    // And they step: the arrows are chrome, but the chrome has to be attached
    // to the thing it claims to do.
    const double before = size->value();
    size->stepBy(1);
    if (!(size->value() > before)) {
        zaro::app::testing::failf("stepping the size field changed nothing\n");
    }
}

// A title that fades in.
//
// The curves a title animates on are the clip's own -- opacity, position -- and
// they have worked on every other kind of clip for a long time. That is exactly
// why this is worth a test: "it should already work on a graphic" is how a
// feature goes missing. What it checks is both halves, the numbers the model
// holds and the picture that comes out of them.
TEST_CASE("A title fades in", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    const auto& sequence = *window.sequence();

    // Somewhere the fixture's own clip is dark, so white glyphs are the only
    // thing that could brighten the frame.
    window.setPosition(zaro::time::RationalTime{10, sequence.frameRate()});
    QApplication::processEvents();
    window.addTitle();
    QApplication::processEvents();

    const auto titleNow = [&window]() -> const zaro::model::Clip* {
        for (const zaro::model::Track& track : window.sequence()->videoTracks()) {
            for (const zaro::model::Clip& clip : track.clips()) {
                if (clip.graphic.kind == zaro::model::GraphicKind::Text) {
                    return &clip;
                }
            }
        }
        return nullptr;
    };
    REQUIRE(titleNow() != nullptr);
    const auto begins = titleNow()->start();
    const double lit = meanGray(settledGrab(window.monitor()));

    const std::size_t stepsBefore = window.commands().position();
    window.animateTitle(zaro::app::commands::TitleMotion::FadeIn);
    QApplication::processEvents();

    const zaro::model::Clip* title = titleNow();
    REQUIRE(title != nullptr);
    const zaro::model::Curve* opacity = title->animation.find(zaro::model::Param::Opacity);
    if (opacity == nullptr || opacity->keyframes().size() != 2) {
        zaro::app::testing::failf("the fade did not write two keyframes\n");
    }
    // Invisible where it starts, and there a moment later.
    if (title->parameterAt(zaro::model::Param::Opacity, begins) > 0.01) {
        zaro::app::testing::failf("the title is not transparent at its first frame\n");
    }
    const auto after = begins + zaro::time::RationalTime{15, sequence.frameRate()};
    if (title->parameterAt(zaro::model::Param::Opacity, after) < 0.99) {
        zaro::app::testing::failf("the title never becomes opaque\n");
    }

    // And the picture agrees: the first frame of the title carries no text.
    const double faded = meanGray(settledGrab(window.monitor()));
    std::printf("  title fade: %.2f at the first frame, %.2f before the fade was added\n", faded,
                lit);
    if (!(faded < lit - 0.05)) {
        zaro::app::testing::failf("the first frame still carries the title (%.2f against %.2f)\n",
                                  faded, lit);
    }

    // One undo step for the pair: a fade is one decision, and half of it is a
    // title that fades in from nothing and never arrives.
    if (window.commands().position() != stepsBefore + 1) {
        zaro::app::testing::failf("the fade took %zu undo steps, not one\n",
                                  window.commands().position() - stepsBefore);
    }
    window.commands().undo(window.project());
    QApplication::processEvents();
    const zaro::model::Clip* back = titleNow();
    if (back == nullptr || back->animation.find(zaro::model::Param::Opacity) != nullptr) {
        zaro::app::testing::failf("undo left the fade behind\n");
    }

    // A preset asked for on something that is not a title is refused rather
    // than quietly animating a shot.
    const auto& pictureTrack = window.sequence()->videoTracks().front();
    if (!pictureTrack.clips().empty() &&
        pictureTrack.clips().front().graphic.kind != zaro::model::GraphicKind::Text) {
        window.timeline()->selectOnly(pictureTrack.id(), pictureTrack.clips().front().id);
        QApplication::processEvents();
        const std::size_t steps = window.commands().position();
        if (Status refused = zaro::app::commands::animateTitle(
                window.editContext(), zaro::app::commands::TitleMotion::FadeIn);
            refused) {
            zaro::app::testing::failf("a shot was given a title's fade\n");
        }
        if (window.commands().position() != steps) {
            zaro::app::testing::failf("the refusal still changed the cut\n");
        }
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    QApplication::processEvents();
}

// A title asking for a font this machine does not have.
//
// The family is stored as a name and resolved when the text is drawn, so a
// project made elsewhere keeps asking for the typeface it was made with and
// gets it back on a machine that has one. The cost is that the substitution is
// invisible: the font list can only show what is installed, so it reads back as
// whichever font Qt chose and the panel quietly agrees with itself. This checks
// the panel says so instead.
TEST_CASE("A missing font is named rather than silently swapped", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    const auto& sequence = *window.sequence();

    window.setPosition(zaro::time::RationalTime{10, sequence.frameRate()});
    QApplication::processEvents();
    window.addTitle();
    QApplication::processEvents();

    auto* note = window.findChild<QLabel*>("text-font-note");
    if (note == nullptr) {
        zaro::app::testing::failf("the inspector has no font note\n");
    }
    // A title made here asks for nothing in particular, which is a font every
    // machine has.
    // Asked of the widget rather than of the screen: whether the inspector's
    // panel is up at all depends on the workspace, and a test that ran after
    // one which switched workspaces would otherwise report a note that is set
    // correctly as missing.
    if (note->isVisibleTo(note->parentWidget())) {
        zaro::app::testing::failf("a title with no family named said one was missing\n");
    }

    const auto titleNow = [&window]() -> const zaro::model::Clip* {
        for (const zaro::model::Track& track : window.sequence()->videoTracks()) {
            for (const zaro::model::Clip& clip : track.clips()) {
                if (clip.graphic.kind == zaro::model::GraphicKind::Text) {
                    return &clip;
                }
            }
        }
        return nullptr;
    };
    const zaro::model::Clip* title = titleNow();
    REQUIRE(title != nullptr);

    // The project now asks for a typeface that certainly is not installed --
    // the same state a cut made on somebody else's machine arrives in.
    zaro::model::Graphic wanting = title->graphic;
    wanting.family = "Nonesuch Grotesk MMXXVI";
    auto built = zaro::edit::makeSetGraphic(
        window.project(), {sequence.id(), window.editContext().track}, title->id, wanting);
    if (!built) {
        zaro::app::testing::failf("%s\n", built.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*built));
    window.effects()->refresh();
    QApplication::processEvents();

    if (!note->isVisibleTo(note->parentWidget())) {
        zaro::app::testing::failf("a missing font was substituted without a word\n");
    }
    if (!note->text().contains("Nonesuch Grotesk MMXXVI")) {
        zaro::app::testing::failf("the note does not name the font that is missing: %s\n",
                                  note->text().toUtf8().constData());
    }
    std::printf("  missing font: %s\n", note->text().toUtf8().constData());

    // The text still draws, in whatever was substituted: a title nobody can
    // read is worse than one in the wrong face.
    if (titleNow()->graphic.family != wanting.family) {
        zaro::app::testing::failf("the project stopped asking for the font it wants\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    QApplication::processEvents();
}

// A title typed on, a character at a time.
//
// The reveal is a parameter like any other -- a curve on the clip, read at the
// frame being drawn -- and the only one that changes what the text says rather
// than what is done to the picture of it. Both graphs read it through the same
// call, so what this checks is the whole path: the preset writes a curve, the
// curve reaches the compositor, and the picture at a third of the way through
// carries less ink than the picture at the end.
TEST_CASE("A title types itself on", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    const auto& sequence = *window.sequence();

    window.setPosition(zaro::time::RationalTime{10, sequence.frameRate()});
    QApplication::processEvents();
    window.addTitle();
    QApplication::processEvents();

    const auto titleNow = [&window]() -> const zaro::model::Clip* {
        for (const zaro::model::Track& track : window.sequence()->videoTracks()) {
            for (const zaro::model::Clip& clip : track.clips()) {
                if (clip.graphic.kind == zaro::model::GraphicKind::Text) {
                    return &clip;
                }
            }
        }
        return nullptr;
    };
    REQUIRE(titleNow() != nullptr);

    // A longer line than "Title", so there is something to type: five glyphs
    // at a third of the way through is one glyph, and one glyph on a 320-wide
    // frame is not a measurable difference.
    zaro::model::Graphic wordy = titleNow()->graphic;
    wordy.text = "MMMMMMMMMMMMMMMM";
    wordy.alignment = -1;
    auto reworded = zaro::edit::makeSetGraphic(
        window.project(), {sequence.id(), window.editContext().track}, titleNow()->id, wordy);
    REQUIRE(reworded.hasValue());
    window.commands().execute(window.project(), std::move(*reworded));
    window.commands().breakMerge();
    QApplication::processEvents();

    const auto begins = titleNow()->start();
    const auto ends = titleNow()->endExclusive();
    const std::size_t stepsBefore = window.commands().position();
    window.animateTitle(zaro::app::commands::TitleMotion::Typewriter);
    QApplication::processEvents();

    const zaro::model::Clip* title = titleNow();
    REQUIRE(title != nullptr);
    const zaro::model::Curve* reveal = title->animation.find(zaro::model::Param::TextReveal);
    if (reveal == nullptr || reveal->keyframes().size() != 2) {
        zaro::app::testing::failf("the typewriter did not write two keyframes\n");
    }
    if (title->parameterAt(zaro::model::Param::TextReveal, begins) > 0.01) {
        zaro::app::testing::failf("the title shows something at its first frame\n");
    }
    // Finished before the clip ends, so the completed line holds.
    const auto nearlyOver = ends - zaro::time::RationalTime{2, sequence.frameRate()};
    if (title->parameterAt(zaro::model::Param::TextReveal, nearlyOver) < 0.99) {
        zaro::app::testing::failf("the title never finishes typing\n");
    }

    // And the picture agrees. Measured a third of the way in against the end,
    // through the real compositor and the real font engine.
    const auto frames = (ends - begins).frames();
    window.setPosition(begins + zaro::time::RationalTime{frames / 3, sequence.frameRate()});
    const double early = meanGray(settledGrab(window.monitor()));
    window.setPosition(nearlyOver);
    const double late = meanGray(settledGrab(window.monitor()));
    std::printf("  typewriter: %.2f a third in, %.2f at the end\n", early, late);
    if (!(late > early + 0.05)) {
        zaro::app::testing::failf("the line does not grow as it is typed (%.2f then %.2f)\n", early,
                                  late);
    }

    if (window.commands().position() != stepsBefore + 1) {
        zaro::app::testing::failf("the typewriter took %zu undo steps, not one\n",
                                  window.commands().position() - stepsBefore);
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    QApplication::processEvents();
}
