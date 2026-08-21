#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"

#include "ModelFixtures.h"

using namespace zaro;
using Catch::Approx;
using zaro::testing::Fixture;

// --- Subclips ---------------------------------------------------------------

TEST_CASE("A subclip is a named range of media, and nothing else", "[edit][subclip]") {
    Fixture f;
    model::Subclip subclip;
    subclip.id = f.project.ids().next<model::SubclipTag>();
    subclip.source = f.longMedia;
    subclip.range = f.range(100, 250);
    subclip.name = "the good bit";
    const model::SubclipId id = f.project.addSubclip(subclip);

    REQUIRE(f.project.subclips().size() == 1);
    const model::Subclip* back = f.project.findSubclip(id);
    REQUIRE(back != nullptr);
    CHECK(back->name == "the good bit");
    CHECK(back->range.start().frames() == 100);

    SECTION("and removing it takes it out of the bin") {
        CHECK(f.project.removeSubclip(id));
        CHECK(f.project.subclips().empty());
        CHECK_FALSE(f.project.removeSubclip(id));
    }
}

TEST_CASE("Placing a subclip makes an ordinary clip", "[edit][subclip]") {
    // The whole design: nothing downstream learns a new concept. What lands on
    // the timeline is a clip, and every operation already knows what to do
    // with one.
    Fixture f;
    model::Subclip subclip;
    subclip.id = f.project.ids().next<model::SubclipTag>();
    subclip.source = f.longMedia;
    subclip.range = f.range(300, 60);
    f.project.addSubclip(subclip);

    model::Clip placed = f.clip(0, 60, 300);
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), placed)));

    const model::Clip& made = f.track(f.v1).clips().front();
    CHECK(made.source == f.longMedia);
    CHECK(made.sourceRange.start().frames() == 300);
    // And it trims like anything else, past the subclip's edges included: the
    // subclip is a note in the bin, not a constraint on the cut.
    REQUIRE(f.run(edit::makeTrim(f.project, f.on(f.v1), made.id, edit::Edge::Out, f.at(30))));
    CHECK(f.track(f.v1).clips().front().sourceRange.duration().frames() == 90);
}

TEST_CASE("Subclips survive a round trip", "[edit][subclip][io]") {
    Fixture f;
    model::Subclip subclip;
    subclip.id = f.project.ids().next<model::SubclipTag>();
    subclip.source = f.longMedia;
    subclip.range = f.range(120, 45);
    subclip.name = "take 3";
    f.project.addSubclip(subclip);

    auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    auto reloaded = io::loadProjectFromString(*text);
    REQUIRE(reloaded);

    REQUIRE(reloaded->project.subclips().size() == 1);
    const model::Subclip& back = reloaded->project.subclips().front();
    CHECK(back.name == "take 3");
    CHECK(back.source == f.longMedia);
    CHECK(back.range.duration().frames() == 45);
    // The id counter has to clear it, or the next thing added reuses its id.
    CHECK(reloaded->project.ids().next<model::SubclipTag>().value() > back.id.value());
}

TEST_CASE("A subclip of media that is not there is dropped on load", "[edit][subclip][io]") {
    // It would show in the bin as something that cannot be opened, which is
    // worse than not showing at all.
    Fixture f;
    model::Subclip orphan;
    orphan.id = f.project.ids().next<model::SubclipTag>();
    orphan.source = model::MediaRefId{9999};
    orphan.range = f.range(0, 10);
    f.project.addSubclip(orphan);

    auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    auto reloaded = io::loadProjectFromString(*text);
    REQUIRE(reloaded);
    CHECK(reloaded->project.subclips().empty());
}

// --- Replacing footage ------------------------------------------------------

TEST_CASE("Replacing footage keeps the cut", "[edit][replace]") {
    Fixture f;
    const model::MediaRefId graded = f.addMedia("graded.mov", 10000);
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 40, 600))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;

    // Something on the clip that must not be lost: replacing footage is for
    // when the edit is right and the material is wrong.
    model::ColorCorrection warm;
    warm.temperature = 20.0;
    REQUIRE(f.run(edit::makeSetColorCorrection(f.project, f.on(f.v1), clipId, warm)));

    REQUIRE(f.run(edit::makeReplaceSource(f.project, f.on(f.v1), clipId, graded)));
    const model::Clip& after = f.track(f.v1).clips().front();
    CHECK(after.source == graded);
    CHECK(after.start().frames() == 50);
    CHECK(after.timelineRange.duration().frames() == 40);
    CHECK(after.sourceRange.start().frames() == 600);
    CHECK(after.color.temperature == Approx(20.0));
}

TEST_CASE("A shorter file slides the in point back rather than shortening the clip",
          "[edit][replace]") {
    Fixture f;
    // 100 frames of media; the clip wants 40 frames starting at 500.
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 40, 500))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;

    REQUIRE(f.run(edit::makeReplaceSource(f.project, f.on(f.v1), clipId, f.shortMedia)));
    const model::Clip& after = f.track(f.v1).clips().front();
    // The last 40 frames of the short file, and the cut untouched.
    CHECK(after.sourceRange.start().frames() == 60);
    CHECK(after.timelineRange.duration().frames() == 40);
}

TEST_CASE("Media shorter than the clip is refused", "[edit][replace]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 200, 0))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;
    // Silently shortening the clip would ripple a cut nobody asked to change.
    CHECK_FALSE(edit::makeReplaceSource(f.project, f.on(f.v1), clipId, f.shortMedia));
}

TEST_CASE("A clip with no media to replace is refused", "[edit][replace]") {
    Fixture f;
    model::Graphic shape;
    shape.kind = model::GraphicKind::Rectangle;
    shape.width = 10.0;
    shape.height = 10.0;
    REQUIRE(f.run(edit::makeAddGraphic(f.project, f.on(f.v1), shape, f.range(0, 20))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;
    CHECK_FALSE(edit::makeReplaceSource(f.project, f.on(f.v1), clipId, f.longMedia));
}

TEST_CASE("Replacing footage is one undoable step", "[edit][replace]") {
    Fixture f;
    const model::MediaRefId other = f.addMedia("other.mov", 10000);
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 40, 500))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;

    REQUIRE(f.run(edit::makeReplaceSource(f.project, f.on(f.v1), clipId, other)));
    f.stack.undo(f.project);
    CHECK(f.track(f.v1).clips().front().source == f.longMedia);
    CHECK(f.track(f.v1).clips().front().sourceRange.start().frames() == 500);
}

// --- Match frame ------------------------------------------------------------

TEST_CASE("Match frame is the clip's own mapping", "[edit][matchframe]") {
    // There is no operation for it: the answer is what the clip already says
    // when asked which frame of its media it is showing. A second
    // implementation of that mapping is a second thing to keep in step with
    // trims, speed, reverse and time remapping.
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(30, 40, 500))));
    const model::Clip& clip = f.track(f.v1).clips().front();

    CHECK(clip.activeSourceTimeAt(f.at(30)).frames() == 500);
    CHECK(clip.activeSourceTimeAt(f.at(45)).frames() == 515);

    SECTION("and it follows a reverse") {
        REQUIRE(f.run(edit::makeSetSpeed(f.project, f.on(f.v1), clip.id, 1.0, true, false)));
        const model::Clip& reversed = f.track(f.v1).clips().front();
        CHECK(reversed.activeSourceTimeAt(f.at(30)).frames() == 539);
    }
}
