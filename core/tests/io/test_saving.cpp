#include <chrono>
#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"

#include "ModelFixtures.h"

using namespace zaro;
using zaro::testing::Fixture;

namespace {

/// A directory that cleans up after itself, so a failing test does not leave
/// files behind for the next run to trip over.
struct TempDir {
    std::filesystem::path path;

    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("zaro-save-test-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code code;
        std::filesystem::remove_all(path, code);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] std::string file(const char* name) const { return (path / name).string(); }
};

}  // namespace

// --- Saving -----------------------------------------------------------------

TEST_CASE("Saving writes beside the file and renames over it", "[io][save]") {
    TempDir dir;
    const std::string path = dir.file("project.zaro");
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));

    REQUIRE(io::saveProject(f.project, path));
    CHECK(std::filesystem::exists(path));
    // No leftovers: a directory littered with half-written files is how people
    // learn not to trust a program with their work.
    CHECK_FALSE(std::filesystem::exists(path + ".saving"));

    auto reloaded = io::loadProject(path);
    REQUIRE(reloaded);
    CHECK(reloaded->project.findSequence(f.sequenceId)->findTrack(f.v1)->clips().size() == 1);
}

TEST_CASE("A save that cannot be written leaves the previous version alone", "[io][save]") {
    TempDir dir;
    const std::string path = dir.file("project.zaro");
    Fixture f;
    REQUIRE(io::saveProject(f.project, path));
    const auto originalSize = std::filesystem::file_size(path);

    // A directory where the file should be: the rename cannot replace it, and
    // the point is that the failure is reported rather than partly applied.
    const std::string blocked = dir.file("blocked.zaro");
    std::filesystem::create_directories(blocked);
    CHECK_FALSE(io::saveProject(f.project, blocked));
    CHECK_FALSE(std::filesystem::exists(blocked + ".saving"));

    // And the good one is untouched.
    CHECK(std::filesystem::file_size(path) == originalSize);
}

TEST_CASE("A recovery file sits beside the project", "[io][save]") {
    CHECK(io::autosavePath("/takes/cut.zaro") == "/takes/cut.zaro.autosave");
}

TEST_CASE("A recovery file is only offered when it is newer", "[io][save]") {
    TempDir dir;
    const std::string path = dir.file("project.zaro");
    Fixture f;

    CHECK_FALSE(io::hasNewerAutosave(path));

    REQUIRE(io::saveProject(f.project, path));
    CHECK_FALSE(io::hasNewerAutosave(path));

    REQUIRE(io::saveProject(f.project, io::autosavePath(path)));
    // Same content, later write: an autosave made after the last real save is
    // the one case where offering it is honest.
    std::filesystem::last_write_time(
        io::autosavePath(path), std::filesystem::last_write_time(path) + std::chrono::seconds{10});
    CHECK(io::hasNewerAutosave(path));

    SECTION("and an older one is not") {
        std::filesystem::last_write_time(
            io::autosavePath(path),
            std::filesystem::last_write_time(path) - std::chrono::seconds{10});
        // It describes work the last real save already includes.
        CHECK_FALSE(io::hasNewerAutosave(path));
    }
}

// --- Knowing what is unsaved ------------------------------------------------

TEST_CASE("A project starts modified and stops when it is saved", "[edit][save]") {
    Fixture f;
    // Never saved is modified: claiming otherwise would be a guess, and the
    // cost of guessing wrong is somebody's work.
    CHECK(f.stack.isModified());

    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    CHECK(f.stack.isModified());

    f.stack.markSaved();
    CHECK_FALSE(f.stack.isModified());

    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(100, 50))));
    CHECK(f.stack.isModified());
}

TEST_CASE("Undoing back to the saved state is not modified", "[edit][save]") {
    // A modified marker that will not go away is one people stop reading.
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    f.stack.markSaved();

    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(100, 50))));
    CHECK(f.stack.isModified());
    f.stack.undo(f.project);
    CHECK_FALSE(f.stack.isModified());

    SECTION("and redoing past it is modified again") {
        f.stack.redo(f.project);
        CHECK(f.stack.isModified());
    }
}

TEST_CASE("A merge that changes nothing about the position still modifies", "[edit][save]") {
    // Dragging a value coalesces into the previous undo step, so the position
    // does not move -- but the project did. Without this, a drag after a save
    // would leave the project reading as unmodified while it had changed.
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;

    model::Transform moved;
    moved.positionX = 10.0;
    REQUIRE(f.run(edit::makeSetTransform(f.project, f.on(f.v1), clipId, moved)));
    f.stack.markSaved();
    CHECK_FALSE(f.stack.isModified());

    moved.positionX = 20.0;
    REQUIRE(f.run(edit::makeSetTransform(f.project, f.on(f.v1), clipId, moved)));
    CHECK(f.stack.isModified());
}

TEST_CASE("A saved state on a discarded branch is no longer recognised", "[edit][save]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(100, 50))));
    f.stack.markSaved();

    f.stack.undo(f.project);
    CHECK(f.stack.isModified());
    // A new command here throws away the branch the saved state was on. There
    // is now no way back to it, so there is no way to recognise it either.
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(200, 50))));
    CHECK(f.stack.isModified());
    f.stack.undo(f.project);
    CHECK(f.stack.isModified());
}

TEST_CASE("A saved state that falls off the end of the history is forgotten", "[edit][save]") {
    Fixture f;
    edit::CommandStack shallow{3};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 20))));
    shallow.markSaved();
    CHECK_FALSE(shallow.isModified());

    for (std::int64_t i = 0; i < 5; ++i) {
        auto built = edit::makeOverwrite(f.project, f.on(f.v1), f.clip(100 + (i * 30), 20));
        REQUIRE(built);
        shallow.execute(f.project, std::move(*built));
        shallow.breakMerge();
    }
    // The state it was saved at has been dropped from the history, so it can no
    // longer be undone back to -- and cannot be claimed as current either.
    CHECK(shallow.isModified());
}

TEST_CASE("Clearing the history forgets where the save was", "[edit][save]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    f.stack.markSaved();
    f.stack.clear();
    // A caller that clears because it has just loaded a file marks the result
    // saved itself; guessing on its behalf would be wrong for every other
    // caller.
    CHECK(f.stack.isModified());
}

// --- Starting from nothing --------------------------------------------------

TEST_CASE("A new project has somewhere to put something", "[edit][newproject]") {
    model::Project project = model::newProject();
    const model::Sequence* sequence = project.findSequence(project.activeSequence());
    REQUIRE(sequence != nullptr);
    // A timeline with no tracks has nowhere to drop anything, and the first
    // thing anybody does is drop something.
    CHECK(sequence->videoTracks().size() == 1);
    CHECK(sequence->audioTracks().size() == 1);
    CHECK(sequence->duration().frames() == 0);

    SECTION("and its ids do not collide with what is added next") {
        const auto next = project.ids().next<model::ClipTag>();
        CHECK(next.value() > sequence->videoTracks().front().id().value());
    }

    SECTION("and it round trips") {
        auto text = io::saveProjectToString(project);
        REQUIRE(text);
        auto reloaded = io::loadProjectFromString(*text);
        REQUIRE(reloaded);
        CHECK(reloaded->project.activeSequence() == project.activeSequence());
    }
}

TEST_CASE("An empty sequence takes its format from what is put on it", "[edit][newproject]") {
    model::Project project = model::newProject();
    const model::SequenceId sequenceId = project.activeSequence();
    edit::CommandStack stack;

    auto built = edit::makeConformSequence(project, sequenceId, time::rates::fps24, 4096, 2160);
    REQUIRE(built);
    stack.execute(project, std::move(*built));

    const model::Sequence* sequence = project.findSequence(sequenceId);
    CHECK(sequence->frameRate() == time::rates::fps24);
    CHECK(sequence->width() == 4096);
    CHECK(sequence->height() == 2160);

    SECTION("and it is undoable like anything else") {
        stack.undo(project);
        CHECK(project.findSequence(sequenceId)->frameRate() == time::rates::fps25);
        CHECK(project.findSequence(sequenceId)->width() == 1920);
    }
}

TEST_CASE("Conforming a sequence that has clips on it is refused", "[edit][newproject]") {
    // Every clip's timeline range is expressed at the sequence's rate, so
    // changing it under a cut would retime the whole thing -- silently, and by
    // a ratio nobody was thinking about.
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    CHECK_FALSE(edit::makeConformSequence(f.project, f.sequenceId, time::rates::fps24, 1920, 1080));

    SECTION("and so is a rate or a size that is not one") {
        model::Project project = model::newProject();
        const model::SequenceId id = project.activeSequence();
        CHECK_FALSE(edit::makeConformSequence(project, id, time::Rational{0, 1}, 1920, 1080));
        CHECK_FALSE(edit::makeConformSequence(project, id, time::rates::fps24, 0, 1080));
    }
}

// --- Interpreting footage ---------------------------------------------------

TEST_CASE("A media reference reads its curve from the file until told otherwise", "[io][color]") {
    Fixture f;
    model::MediaRef& ref = f.project.mediaMutable().front();
    ref.info.videoStreams.front().color.transfer = media::TransferFunction::BT709;
    CHECK(ref.transfer() == media::TransferFunction::BT709);

    // A camera log file says BT.709 because a container has a number for that
    // and none for S-Log3. Nothing in the pixels can tell them apart.
    ref.transferOverride = media::TransferFunction::SLog3;
    CHECK(ref.transfer() == media::TransferFunction::SLog3);

    SECTION("and it survives a round trip") {
        auto text = io::saveProjectToString(f.project);
        REQUIRE(text);
        auto reloaded = io::loadProjectFromString(*text);
        REQUIRE(reloaded);
        CHECK(reloaded->project.media().front().transferOverride == media::TransferFunction::SLog3);
    }

    SECTION("and a file described correctly carries no override") {
        ref.transferOverride = media::TransferFunction::Unknown;
        auto text = io::saveProjectToString(f.project);
        REQUIRE(text);
        CHECK(text->find("transferOverride") == std::string::npos);
    }
}

TEST_CASE("A sequence's delivery curve survives a round trip", "[io][tonemap]") {
    Fixture f;
    model::Sequence::Output delivery;
    delivery.transfer = media::TransferFunction::PQ;
    delivery.highlightKnee = 0.75;
    f.sequence().setOutput(delivery);

    auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    auto reloaded = io::loadProjectFromString(*text);
    REQUIRE(reloaded);
    const model::Sequence::Output& back = reloaded->project.findSequence(f.sequenceId)->output();
    CHECK(back.transfer == media::TransferFunction::PQ);
    CHECK(back.highlightKnee == 0.75);

    SECTION("and a sequence delivered the way every project always was writes nothing") {
        Fixture plain;
        auto bare = io::saveProjectToString(plain.project);
        REQUIRE(bare);
        CHECK(bare->find("\"output\"") == std::string::npos);
    }
}
