#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/io/ProjectLock.h"

using namespace zaro;

namespace {

struct Folder {
    std::filesystem::path root;

    Folder() {
        static int counter = 0;
        root = std::filesystem::temp_directory_path() / ("zaro-locks-" + std::to_string(++counter));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
    }
    ~Folder() {
        // The non-throwing overload: a destructor that throws terminates the
        // process, and Windows refuses to unlink a file some handle is still
        // holding -- which would turn a readable test failure into a crash.
        std::error_code code;
        std::filesystem::remove_all(root, code);
    }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;

    [[nodiscard]] std::string project() const { return (root / "cut.zaro").string(); }
};

}  // namespace

TEST_CASE("a project with nobody in it has no lock", "[lock]") {
    Folder folder;
    auto lock = io::readLock(folder.project());
    CHECK_FALSE(lock);
    CHECK(lock.error().code() == ErrorCode::NotFound);
}

TEST_CASE("a lock says who has it and reads back", "[lock]") {
    Folder folder;
    const io::ProjectLock mine = io::thisProcess();
    REQUIRE(io::writeLock(folder.project(), mine));
    CHECK(std::filesystem::exists(io::lockPath(folder.project())));

    auto read = io::readLock(folder.project());
    REQUIRE(read);
    CHECK(read->user == mine.user);
    CHECK(read->host == mine.host);
    CHECK(read->pid == mine.pid);
    CHECK(io::isOurs(*read));
    // Ours is not stale, whatever else is true: this process is running.
    CHECK_FALSE(io::isStale(*read));
}

TEST_CASE("a lock from a process that has gone is stale", "[lock]") {
    Folder folder;
    io::ProjectLock dead = io::thisProcess();
    // A pid above the system maximum cannot be running, which is the case a
    // crash leaves behind.
    dead.pid = 99999999;
    REQUIRE(io::writeLock(folder.project(), dead));

    auto read = io::readLock(folder.project());
    REQUIRE(read);
    CHECK(io::isStale(*read));
    CHECK_FALSE(io::isOurs(*read));
}

TEST_CASE("a lock from another machine is never called stale", "[lock]") {
    Folder folder;
    io::ProjectLock elsewhere = io::thisProcess();
    elsewhere.host = "some-other-machine";
    elsewhere.pid = 99999999;
    REQUIRE(io::writeLock(folder.project(), elsewhere));

    auto read = io::readLock(folder.project());
    REQUIRE(read);
    // A process id means nothing on another host. Calling this stale would be
    // stealing a lock from somebody whose machine was merely slow to answer.
    CHECK_FALSE(io::isStale(*read));
    CHECK_FALSE(io::isOurs(*read));
}

TEST_CASE("a lock nobody can parse is treated as somebody's", "[lock]") {
    Folder folder;
    {
        std::ofstream file{io::lockPath(folder.project())};
        file << "this is not json";
    }
    auto read = io::readLock(folder.project());
    // The safe reading of "I do not understand this" is that another program
    // wrote it, not that the project is free.
    REQUIRE(read);
    CHECK_FALSE(io::isOurs(*read));
    CHECK_FALSE(io::isStale(*read));
}

TEST_CASE("only our own lock is removed without forcing", "[lock]") {
    Folder folder;
    io::ProjectLock theirs = io::thisProcess();
    theirs.host = "some-other-machine";
    REQUIRE(io::writeLock(folder.project(), theirs));

    CHECK_FALSE(io::removeLock(folder.project()));
    CHECK(std::filesystem::exists(io::lockPath(folder.project())));

    // Taking over is possible, but it has to be asked for.
    CHECK(io::removeLock(folder.project(), true));
    CHECK_FALSE(std::filesystem::exists(io::lockPath(folder.project())));
}

TEST_CASE("removing a lock that is not there is not an error", "[lock]") {
    Folder folder;
    CHECK(io::removeLock(folder.project()));
}
