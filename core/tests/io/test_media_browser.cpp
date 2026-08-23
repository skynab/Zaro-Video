#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/io/MediaBrowser.h"

using namespace zaro;

namespace {

struct Card {
    std::filesystem::path root;

    Card() {
        static int counter = 0;
        root =
            std::filesystem::temp_directory_path() / ("zaro-browser-" + std::to_string(++counter));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
    }
    ~Card() { std::filesystem::remove_all(root); }
    Card(const Card&) = delete;
    Card& operator=(const Card&) = delete;

    void file(const std::string& name) {
        const std::filesystem::path path = root / name;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out{path};
        out << "x";
    }
    void folder(const std::string& name) { std::filesystem::create_directories(root / name); }
};

}  // namespace

TEST_CASE("a folder lists its folders first, then its media", "[browser]") {
    Card card;
    card.file("b_shot.mov");
    card.file("a_shot.MP4");
    card.folder("Zed");
    card.folder("Alpha");

    auto listed = io::listFolder(card.root.string());
    REQUIRE(listed);
    REQUIRE(listed->size() == 4);
    CHECK(listed->at(0).name == "Alpha");
    CHECK(listed->at(0).isFolder);
    CHECK(listed->at(1).name == "Zed");
    // Files after folders, sorted without caring about case.
    CHECK(listed->at(2).name == "a_shot.MP4");
    CHECK(listed->at(3).name == "b_shot.mov");
    CHECK_FALSE(listed->at(3).isFolder);
}

TEST_CASE("what cannot be opened is not listed", "[browser]") {
    Card card;
    card.file("shot.mov");
    card.file("shot.THM");      // a camera thumbnail
    card.file("MEDIAPRO.XML");  // a card manifest
    card.file("notes.txt");

    auto listed = io::listFolder(card.root.string());
    REQUIRE(listed);
    REQUIRE(listed->size() == 1);
    CHECK(listed->front().name == "shot.mov");
}

TEST_CASE("hidden files stay hidden", "[browser]") {
    Card card;
    card.file(".DS_Store");
    card.file("._shot.mov");
    card.file("shot.mov");
    card.folder(".Trashes");

    auto listed = io::listFolder(card.root.string());
    REQUIRE(listed);
    REQUIRE(listed->size() == 1);
    CHECK(listed->front().name == "shot.mov");
}

TEST_CASE("a listed file carries its size", "[browser]") {
    Card card;
    card.file("shot.mov");
    auto listed = io::listFolder(card.root.string());
    REQUIRE(listed);
    REQUIRE(listed->size() == 1);
    CHECK(listed->front().bytes == 1);
    CHECK(listed->front().path == (card.root / "shot.mov").string());
}

TEST_CASE("a folder that is not there is refused by name", "[browser]") {
    Card card;
    auto listed = io::listFolder((card.root / "nope").string());
    CHECK_FALSE(listed);
    CHECK(listed.error().code() == ErrorCode::NotFound);
}

TEST_CASE("extensions are matched whatever the case", "[browser]") {
    CHECK(io::looksLikeMedia("/cards/A001.MOV"));
    CHECK(io::looksLikeMedia("/cards/a001.mov"));
    CHECK(io::looksLikeMedia("/sound/take1.WAV"));
    CHECK_FALSE(io::looksLikeMedia("/cards/A001.xml"));
    CHECK_FALSE(io::looksLikeMedia("/cards/A001"));
}
