#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"

#include "ModelFixtures.h"

using namespace zaro;
using zaro::testing::Fixture;

namespace {

/// A project with enough shape that a round trip has something to lose.
Fixture populated() {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 600))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v2), f.clip(25, 100, 700))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 150, 800))));
    f.track(f.v2).setMuted(true);
    f.track(f.a1).setLocked(true);
    f.sequence().setStartTime(time::RationalTime{107892, time::rates::fps25});
    return f;
}

}  // namespace

TEST_CASE("A project survives a round trip unchanged", "[io]") {
    Fixture f = populated();

    const auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    const auto loaded = io::loadProjectFromString(*text);
    REQUIRE(loaded);

    CHECK(loaded->project == f.project);

    SECTION("and saving the loaded copy produces the identical bytes") {
        const auto again = io::saveProjectToString(loaded->project, loaded->unknown);
        REQUIRE(again);
        CHECK(*again == *text);
    }
}

TEST_CASE("Rates survive as exact fractions, not decimals", "[io]") {
    Fixture f;
    // Name deliberately free of digits: the assertion below is that no rounded
    // rate appears anywhere in the file, and a name would be a false positive.
    model::Sequence broadcast{f.project.ids().next<model::SequenceTag>(), "Broadcast master",
                              time::rates::fps29_97};
    const model::SequenceId id = broadcast.id();
    f.project.addSequence(std::move(broadcast));

    const auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    // The file must not contain a rounded rate anywhere.
    CHECK(text->find("30000/1001") != std::string::npos);
    CHECK(text->find("29.97") == std::string::npos);

    const auto loaded = io::loadProjectFromString(*text);
    REQUIRE(loaded);
    CHECK(loaded->project.findSequence(id)->frameRate() == time::rates::fps29_97);
}

TEST_CASE("Ids are stable across save and load", "[io]") {
    Fixture f = populated();
    const model::ClipId firstClip = f.track(f.v1).clips()[0].id;

    const auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    auto loaded = io::loadProjectFromString(*text);
    REQUIRE(loaded);

    const model::Sequence* sequence = loaded->project.findSequence(f.sequenceId);
    REQUIRE(sequence != nullptr);
    REQUIRE(sequence->findTrack(f.v1) != nullptr);
    CHECK(sequence->findTrack(f.v1)->clips()[0].id == firstClip);

    SECTION("and new ids do not collide with loaded ones") {
        const auto fresh = loaded->project.ids().next<model::ClipTag>();
        CHECK(fresh.value() > firstClip.value());
        for (const model::Sequence& s : loaded->project.sequences()) {
            for (const model::Track& t : s.videoTracks()) {
                for (const model::Clip& c : t.clips()) {
                    CHECK(c.id != fresh);
                }
            }
        }
    }
}

TEST_CASE("Track flags and sequence settings round trip", "[io]") {
    Fixture f = populated();
    const auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    const auto loaded = io::loadProjectFromString(*text);
    REQUIRE(loaded);

    const model::Sequence* sequence = loaded->project.findSequence(f.sequenceId);
    REQUIRE(sequence != nullptr);
    CHECK(sequence->findTrack(f.v2)->isMuted());
    CHECK(sequence->findTrack(f.a1)->isLocked());
    CHECK(sequence->startTime().frames() == 107892);
    CHECK(sequence->width() == 1920);
    CHECK(sequence->audioSampleRate() == time::rates::hz48000);
}

TEST_CASE("Fields written by a newer build are not destroyed", "[io]") {
    // The scenario: someone opens a project in an older build and saves it.
    // Everything the newer build added has to still be there afterwards.
    Fixture f = populated();
    const auto text = io::saveProjectToString(f.project);
    REQUIRE(text);

    std::string enriched = *text;
    const auto injectAfter = [&enriched](const std::string& anchor, const std::string& addition) {
        const auto at = enriched.find(anchor);
        REQUIRE(at != std::string::npos);
        enriched.insert(at + anchor.size(), addition);
    };
    injectAfter("\"activeSequence\":", "");
    injectAfter("{\n", "  \"effectsFromTheFuture\": [\"lumetri\", \"warp\"],\n");

    const auto loaded = io::loadProjectFromString(enriched);
    REQUIRE(loaded);
    const auto resaved = io::saveProjectToString(loaded->project, loaded->unknown);
    REQUIRE(resaved);

    CHECK(resaved->find("effectsFromTheFuture") != std::string::npos);
    CHECK(resaved->find("lumetri") != std::string::npos);

    SECTION("and dropping the carrier does lose them, which is why it exists") {
        const auto without = io::saveProjectToString(loaded->project);
        REQUIRE(without);
        CHECK(without->find("effectsFromTheFuture") == std::string::npos);
    }
}

TEST_CASE("Unknown fields on a clip follow the clip", "[io]") {
    Fixture f = populated();
    const auto text = io::saveProjectToString(f.project);
    REQUIRE(text);

    // Attach something to the first clip, then reorder the timeline so the clip
    // is no longer at the same array index.
    std::string enriched = *text;
    const auto clipAt = enriched.find("\"sourceRange\"");
    REQUIRE(clipAt != std::string::npos);
    enriched.insert(clipAt, "\"speed\": \"1/2\",\n          ");

    auto loaded = io::loadProjectFromString(enriched);
    REQUIRE(loaded);

    edit::CommandStack stack;
    const model::ClipId first =
        loaded->project.findSequence(f.sequenceId)->findTrack(f.v1)->clips()[0].id;
    auto moved = edit::makeMove(loaded->project, {f.sequenceId, f.v1}, first, f.v1,
                                time::RationalTime{500, time::rates::fps25});
    REQUIRE(moved);
    stack.execute(loaded->project, std::move(*moved));

    const auto resaved = io::saveProjectToString(loaded->project, loaded->unknown);
    REQUIRE(resaved);
    // Matched by id rather than by position, so the reorder does not lose it.
    CHECK(resaved->find("\"speed\"") != std::string::npos);
}

TEST_CASE("Malformed input is rejected with something readable", "[io]") {
    SECTION("not JSON at all") {
        const auto loaded = io::loadProjectFromString("this is not json");
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().message().find("valid JSON") != std::string::npos);
    }

    SECTION("JSON, but not a project") {
        const auto loaded = io::loadProjectFromString(R"({"something": "else"})");
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().message().find("Zaro project") != std::string::npos);
    }

    SECTION("a project with no schema version") {
        const auto loaded = io::loadProjectFromString(R"({"zaro": {}})");
        REQUIRE_FALSE(loaded);
    }

    SECTION("a clip with no id") {
        const auto loaded = io::loadProjectFromString(R"({
            "zaro": {"schemaVersion": 1},
            "sequences": [{"id": 1, "frameRate": "25", "videoTracks": [
                {"id": 2, "clips": [{"sourceRange": {"start": {"frames": 0, "rate": "25"},
                                                     "duration": {"frames": 10, "rate": "25"}},
                                     "timelineRange": {"start": {"frames": 0, "rate": "25"},
                                                       "duration": {"frames": 10, "rate": "25"}}}]}
            ]}]})");
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().message().find("no id") != std::string::npos);
    }

    SECTION("a rate that is not a rate") {
        const auto loaded = io::loadProjectFromString(R"({
            "zaro": {"schemaVersion": 1},
            "sequences": [{"id": 1, "frameRate": "sometimes"}]})");
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().message().find("as a rate") != std::string::npos);
    }
}

TEST_CASE("Writing and reading a file", "[io]") {
    Fixture f = populated();
    const std::string path = std::string{ZARO_SCRATCH_DIR} + "/roundtrip.zaro";

    REQUIRE(io::saveProject(f.project, path).ok());
    const auto loaded = io::loadProject(path);
    REQUIRE(loaded);
    CHECK(loaded->project == f.project);

    SECTION("a missing file reports not found") {
        const auto missing = io::loadProject(path + ".nope");
        REQUIRE_FALSE(missing);
        CHECK(missing.error().code() == ErrorCode::NotFound);
    }
}
