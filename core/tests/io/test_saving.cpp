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
