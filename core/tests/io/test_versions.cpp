#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/io/ProjectIo.h"

using namespace zaro;

namespace {

struct Folder {
    std::filesystem::path root;

    Folder() {
        static int counter = 0;
        root =
            std::filesystem::temp_directory_path() / ("zaro-versions-" + std::to_string(++counter));
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

    std::string touch(const std::string& name) {
        const std::filesystem::path path = root / name;
        std::ofstream file{path};
        file << "{}";
        return path.string();
    }
    [[nodiscard]] std::string at(const std::string& name) const { return (root / name).string(); }
};

std::string nameOf(const std::string& path) {
    return std::filesystem::path{path}.filename().string();
}

}  // namespace

TEST_CASE("the first new version of an unnumbered project is v002", "[versions]") {
    Folder folder;
    const std::string first = folder.touch("cut.zaro");
    // The unnumbered file is version one of itself, so the next one is two.
    CHECK(nameOf(io::nextVersionPath(first)) == "cut_v002.zaro");
}

TEST_CASE("a numbered project counts on from the highest beside it", "[versions]") {
    Folder folder;
    folder.touch("cut.zaro");
    folder.touch("cut_v002.zaro");
    const std::string third = folder.touch("cut_v003.zaro");
    folder.touch("cut_v005.zaro");

    // Working on v003 while v005 exists: a new version that reused v004 would
    // be fine, but one that reused v005 would destroy work. The highest plus
    // one is the answer that cannot.
    CHECK(nameOf(io::nextVersionPath(third)) == "cut_v006.zaro");
}

TEST_CASE("the numbering width somebody chose is kept", "[versions]") {
    Folder folder;
    const std::string second = folder.touch("cut_v02.zaro");
    CHECK(nameOf(io::nextVersionPath(second)) == "cut_v03.zaro");
}

TEST_CASE("a name that merely contains digits is not a version", "[versions]") {
    Folder folder;
    const std::string odd = folder.touch("take2.zaro");
    // "take2" is a name, not version two of "take": renumbering it would be
    // this tool having an opinion about somebody's filing.
    CHECK(nameOf(io::nextVersionPath(odd)) == "take2_v002.zaro");
}

TEST_CASE("versions of other projects in the same folder are ignored", "[versions]") {
    Folder folder;
    const std::string ours = folder.touch("cut.zaro");
    folder.touch("trailer_v009.zaro");
    CHECK(nameOf(io::nextVersionPath(ours)) == "cut_v002.zaro");
}

TEST_CASE("the versions of a project are listed oldest first", "[versions]") {
    Folder folder;
    folder.touch("cut_v003.zaro");
    folder.touch("cut.zaro");
    folder.touch("cut_v002.zaro");
    folder.touch("other.zaro");

    const std::vector<std::string> found = io::versionsOf(folder.at("cut_v002.zaro"));
    REQUIRE(found.size() == 3);
    CHECK(nameOf(found[0]) == "cut.zaro");
    CHECK(nameOf(found[1]) == "cut_v002.zaro");
    CHECK(nameOf(found[2]) == "cut_v003.zaro");
}

TEST_CASE("saving a new version writes a file that opens", "[versions]") {
    Folder folder;
    model::Project project;
    model::Sequence sequence{project.ids().next<model::SequenceTag>(), "Sequence",
                             time::rates::fps25};
    const auto sequenceId = sequence.id();
    project.addSequence(std::move(sequence));

    const std::string first = folder.at("cut.zaro");
    REQUIRE(io::saveProject(project, first));

    const std::string second = io::nextVersionPath(first);
    REQUIRE(io::saveProject(project, second));
    CHECK(std::filesystem::exists(second));
    // The previous version is still there: that is the point of versioning.
    CHECK(std::filesystem::exists(first));

    auto reopened = io::loadProject(second);
    REQUIRE(reopened);
    CHECK(reopened->project.findSequence(sequenceId) != nullptr);

    // And the one after that steps past both.
    CHECK(nameOf(io::nextVersionPath(second)) == "cut_v003.zaro");
}
