// Titles, captions and the things pinned to a shot.
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
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

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
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

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
    const double withCaption = meanGray(window.monitor()->grabFramebuffer());

    window.commands().undo(window.project());
    window.monitor()->update();
    QApplication::processEvents();
    const double without = meanGray(window.monitor()->grabFramebuffer());

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
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

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
    const double blank = meanGray(window.monitor()->grabFramebuffer());

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
    const double withText = meanGray(window.monitor()->grabFramebuffer());

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
    [[maybe_unused]] const auto& meanGray = zaro::app::testing::meanGray;

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
