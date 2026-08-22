#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/Relink.h"
#include "zaro/core/media/Waveform.h"

using namespace zaro;

namespace {

/// A scratch tree that cleans up after itself, so a failing test does not
/// leave a folder of rubbish behind for the next run to find.
struct Scratch {
    std::filesystem::path root;

    Scratch() {
        static int counter = 0;
        root =
            std::filesystem::temp_directory_path() / ("zaro-relink-" + std::to_string(++counter));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
    }
    ~Scratch() { std::filesystem::remove_all(root); }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    std::string write(const std::string& relative, const std::string& contents) {
        const std::filesystem::path path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file{path, std::ios::binary};
        file << contents;
        return path.string();
    }
};

model::MediaRefId addMedia(model::Project& project, const std::string& path,
                           const std::string& digest) {
    model::MediaRef ref;
    ref.id = project.ids().next<model::MediaRefTag>();
    ref.path = path;
    ref.name = std::filesystem::path{path}.filename().string();
    ref.contentDigest = digest;
    return project.addMedia(ref);
}

}  // namespace

TEST_CASE("media that is where the project says it is is left alone", "[relink]") {
    Scratch scratch;
    const std::string here = scratch.write("shot.mov", "the picture");

    model::Project project;
    addMedia(project, here, {});

    auto report = io::findRelinks(project, scratch.root.string());
    REQUIRE(report);
    CHECK(report->matches.empty());
    CHECK(report->stillMissing.empty());
}

TEST_CASE("a moved file is found by name and confirmed by content", "[relink]") {
    Scratch scratch;
    const std::string was = (scratch.root / "old" / "shot.mov").string();
    const std::string now = scratch.write("new/deeper/shot.mov", "the picture");
    auto digest = media::contentDigest(now);
    REQUIRE(digest);

    model::Project project;
    const auto id = addMedia(project, was, *digest);

    auto report = io::findRelinks(project, scratch.root.string());
    REQUIRE(report);
    REQUIRE(report->matches.size() == 1);
    CHECK(report->matches.front().media == id);
    CHECK(report->matches.front().found == now);
    CHECK(report->matches.front().byContent);
    CHECK(report->stillMissing.empty());
    CHECK(report->examined > 0);
}

TEST_CASE("the file whose content matches wins over the one that only shares a name", "[relink]") {
    Scratch scratch;
    const std::string decoy = scratch.write("a-first/shot.mov", "a different take entirely");
    const std::string real = scratch.write("z-last/shot.mov", "the picture");
    static_cast<void>(decoy);
    auto digest = media::contentDigest(real);
    REQUIRE(digest);

    model::Project project;
    addMedia(project, (scratch.root / "gone" / "shot.mov").string(), *digest);

    auto report = io::findRelinks(project, scratch.root.string());
    REQUIRE(report);
    REQUIRE(report->matches.size() == 1);
    // Sorted order would have picked the decoy: content is what decides.
    CHECK(report->matches.front().found == real);
    CHECK(report->matches.front().byContent);
}

TEST_CASE("a name match with no digest to check is offered but not claimed as certain",
          "[relink]") {
    Scratch scratch;
    const std::string now = scratch.write("somewhere/shot.mov", "the picture");

    model::Project project;
    addMedia(project, (scratch.root / "gone" / "shot.mov").string(), {});

    auto report = io::findRelinks(project, scratch.root.string());
    REQUIRE(report);
    REQUIRE(report->matches.size() == 1);
    CHECK(report->matches.front().found == now);
    CHECK_FALSE(report->matches.front().byContent);
}

TEST_CASE("a file that is not under the folder is reported as still missing", "[relink]") {
    Scratch scratch;
    scratch.write("something/else.mov", "not it");

    model::Project project;
    const auto id = addMedia(project, (scratch.root / "gone" / "shot.mov").string(), {});

    auto report = io::findRelinks(project, scratch.root.string());
    REQUIRE(report);
    CHECK(report->matches.empty());
    REQUIRE(report->stillMissing.size() == 1);
    CHECK(report->stillMissing.front() == id);
}

TEST_CASE("relinking points the project at the new file and re-takes its digest", "[relink]") {
    Scratch scratch;
    const std::string now = scratch.write("new/shot.mov", "the picture");

    model::Project project;
    const auto id = addMedia(project, (scratch.root / "gone" / "shot.mov").string(), {});
    edit::CommandStack commands;

    auto built = edit::makeRelinkMedia(project, id, now);
    REQUIRE(built);
    commands.execute(project, std::move(*built));

    const model::MediaRef* media = project.findMedia(id);
    REQUIRE(media != nullptr);
    CHECK(media->path == now);
    CHECK_FALSE(media->contentDigest.empty());

    // And it undoes, like every other edit.
    commands.undo(project);
    CHECK(project.findMedia(id)->path != now);
}

TEST_CASE("relinking to a file that is not there is refused", "[relink]") {
    Scratch scratch;
    model::Project project;
    const auto id = addMedia(project, (scratch.root / "gone" / "shot.mov").string(), {});
    CHECK_FALSE(edit::makeRelinkMedia(project, id, (scratch.root / "nope.mov").string()));
}

TEST_CASE("consolidating gathers every file into one folder", "[relink][consolidate]") {
    Scratch scratch;
    const std::string first = scratch.write("cards/a/shot.mov", "the first take");
    const std::string second = scratch.write("cards/b/other.mov", "another one");
    const std::filesystem::path into = scratch.root / "gathered";

    model::Project project;
    const auto firstId = addMedia(project, first, {});
    const auto secondId = addMedia(project, second, {});

    auto report = io::consolidate(project, into.string());
    REQUIRE(report);
    REQUIRE(report->files.size() == 2);
    CHECK(report->missing.empty());
    CHECK(report->bytes > 0);
    for (const io::ConsolidatedFile& file : report->files) {
        CHECK(std::filesystem::path{file.to}.parent_path() == into);
        CHECK(std::filesystem::exists(file.to));
        // Copied, never moved: the original is somebody's rushes.
        CHECK(std::filesystem::exists(file.from));
    }
    static_cast<void>(firstId);
    static_cast<void>(secondId);
}

TEST_CASE("two files with the same name do not overwrite each other", "[relink][consolidate]") {
    Scratch scratch;
    const std::string fromCardA = scratch.write("a/C0001.MP4", "card A");
    const std::string fromCardB = scratch.write("b/C0001.MP4", "card B");
    const std::filesystem::path into = scratch.root / "gathered";

    model::Project project;
    addMedia(project, fromCardA, {});
    addMedia(project, fromCardB, {});

    auto report = io::consolidate(project, into.string());
    REQUIRE(report);
    REQUIRE(report->files.size() == 2);
    CHECK(report->files[0].to != report->files[1].to);
    // The suffix goes before the extension, so the file is still a .MP4 to
    // everything that reads names.
    CHECK(std::filesystem::path{report->files[1].to}.extension() == ".MP4");

    const auto contentsOf = [](const std::string& path) {
        std::ifstream file{path, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    };
    CHECK(contentsOf(report->files[0].to) == "card A");
    CHECK(contentsOf(report->files[1].to) == "card B");
}

TEST_CASE("a file already in the folder is left where it is", "[relink][consolidate]") {
    Scratch scratch;
    const std::filesystem::path into = scratch.root / "gathered";
    std::filesystem::create_directories(into);
    const std::string already = scratch.write("gathered/shot.mov", "already here");

    model::Project project;
    addMedia(project, already, {});

    auto report = io::consolidate(project, into.string());
    REQUIRE(report);
    REQUIRE(report->files.size() == 1);
    CHECK(report->files.front().alreadyThere);
    CHECK(report->files.front().to == already);
    CHECK(report->bytes == 0);
    // Consolidating twice must not produce shot-2.mov.
    auto again = io::consolidate(project, into.string());
    REQUIRE(again);
    CHECK(again->files.front().to == already);
    CHECK(std::distance(std::filesystem::directory_iterator{into},
                        std::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("media that is not there is reported rather than gathered", "[relink][consolidate]") {
    Scratch scratch;
    const std::filesystem::path into = scratch.root / "gathered";

    model::Project project;
    const auto id = addMedia(project, (scratch.root / "gone" / "shot.mov").string(), {});

    auto report = io::consolidate(project, into.string());
    REQUIRE(report);
    CHECK(report->files.empty());
    REQUIRE(report->missing.size() == 1);
    CHECK(report->missing.front() == id);
}
