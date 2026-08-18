#include <array>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"

#include "ModelFixtures.h"

using namespace zaro;
using edit::Edge;
using zaro::testing::Fixture;

namespace {

/// Everything that must be true of a sequence no matter what was done to it.
///
/// Checked after every single operation in the fuzzer. Most edit bugs show up
/// here long before they show up as a wrong picture: an overlap, a clip of
/// negative length, a source range that reaches past the end of the file.
std::string findViolation(const model::Project& project, const model::Sequence& sequence) {
    std::set<std::uint64_t> seenIds;

    for (const auto* list : {&sequence.videoTracks(), &sequence.audioTracks()}) {
        for (const model::Track& track : *list) {
            const model::Clip* previous = nullptr;
            for (const model::Clip& clip : track.clips()) {
                const std::string where =
                    "track '" + track.name() + "' clip " + std::to_string(clip.id.value());

                if (!clip.id.isValid()) {
                    return where + " has no id";
                }
                if (!seenIds.insert(clip.id.value()).second) {
                    return where + " has a duplicate id";
                }
                if (clip.duration().frames() <= 0) {
                    return where + " has a non-positive duration";
                }
                if (clip.sourceRange.duration().frames() <= 0) {
                    return where + " references no source";
                }
                if (clip.start().frames() < 0) {
                    return where + " starts before the sequence";
                }
                if (previous != nullptr) {
                    if (clip.start() < previous->endExclusive()) {
                        return where + " overlaps the clip before it";
                    }
                }
                if (const model::MediaRef* ref = project.findMedia(clip.source)) {
                    if (ref->info.duration.isPositive()) {
                        const time::RationalTime available = time::RationalTime::fromSeconds(
                            ref->info.duration, clip.sourceRange.start().rate());
                        if (clip.sourceRange.start().frames() < 0) {
                            return where + " reads before the start of its source";
                        }
                        if (clip.sourceRange.endExclusive() > available) {
                            return where + " reads past the end of its source";
                        }
                    }
                }
                previous = &clip;
            }
        }
    }
    return {};
}

/// A compact, comparable rendering of the whole sequence, for equality checks
/// that report a readable difference instead of just "not equal".
std::string render(const model::Sequence& sequence) {
    std::string out;
    for (const auto* list : {&sequence.videoTracks(), &sequence.audioTracks()}) {
        for (const model::Track& track : *list) {
            out += track.name() + ":";
            for (const model::Clip& clip : track.clips()) {
                out += " " + std::to_string(clip.id.value()) + "[" +
                       std::to_string(clip.start().frames()) + "-" +
                       std::to_string(clip.endExclusive().frames()) + "@" +
                       std::to_string(clip.sourceRange.start().frames()) + "]";
            }
            out += "\n";
        }
    }
    return out;
}

}  // namespace

TEST_CASE("Twenty edits, undone to empty, redone, saved and reloaded", "[edit][fuzz]") {
    // The Phase 2 exit criterion, spelled out.
    Fixture f;
    std::vector<std::string> statesAfterEachEdit;

    const auto record = [&] { statesAfterEachEdit.push_back(render(f.sequence())); };

    ZARO_REQUIRE_EDIT(f, edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 60, 500)));
    record();
    ZARO_REQUIRE_EDIT(f, edit::makeOverwrite(f.project, f.on(f.v1), f.clip(60, 60, 600)));
    record();
    ZARO_REQUIRE_EDIT(f, edit::makeOverwrite(f.project, f.on(f.v1), f.clip(120, 60, 700)));
    record();
    ZARO_REQUIRE_EDIT(f, edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 180, 800)));
    record();
    ZARO_REQUIRE_EDIT(f, edit::makeRazor(f.project, f.on(f.v1), f.at(30)));
    record();
    ZARO_REQUIRE_EDIT(f, edit::makeRazor(f.project, f.on(f.a1), f.at(90)));
    record();

    // Roll and slide need their neighbours to abut, so they go first, while V1
    // is still one continuous run of clips.
    const model::ClipId second = f.track(f.v1).clips()[1].id;
    ZARO_REQUIRE_EDIT(f, edit::makeRoll(f.project, f.on(f.v1), second, f.at(7)));
    record();
    ZARO_REQUIRE_EDIT(f, edit::makeSlip(f.project, f.on(f.v1), second, f.at(10)));
    record();
    ZARO_REQUIRE_EDIT(
        f, edit::makeSlide(f.project, f.on(f.v1), f.track(f.v1).clips()[2].id, f.at(-4)));
    record();

    // From here on the track has gaps in it, which is the point: the remaining
    // operations have to cope with a timeline that is not tidy.
    ZARO_REQUIRE_EDIT(f, edit::makeTrim(f.project, f.on(f.v1), second, Edge::Out, f.at(-5)));
    record();
    ZARO_REQUIRE_EDIT(f, edit::makeRippleTrim(f.project, f.on(f.v1), second, Edge::Out, f.at(5)));
    record();
    ZARO_REQUIRE_EDIT(f, edit::makeLift(f.project, f.on(f.v1), f.track(f.v1).clips()[2].id));
    record();
    ZARO_REQUIRE_EDIT(f, edit::makeOverwrite(f.project, f.on(f.v2), f.clip(20, 80, 900)));
    record();
    ZARO_REQUIRE_EDIT(f, edit::makeInsert(f.project, f.on(f.v1), f.clip(10, 20, 1000)));
    record();
    ZARO_REQUIRE_EDIT(f, edit::makeInsert(f.project, f.on(f.v1), f.clip(0, 15, 1100), true));
    record();
    ZARO_REQUIRE_EDIT(f, edit::makeRippleDelete(f.project, f.on(f.v1), f.range(40, 20)));
    record();
    ZARO_REQUIRE_EDIT(f,
                      edit::makeAddTrack(f.project, f.sequenceId, model::TrackKind::Video, "V3"));
    record();

    const model::ClipId onV2 = f.track(f.v2).clips()[0].id;
    ZARO_REQUIRE_EDIT(f, edit::makeMove(f.project, f.on(f.v2), onV2, f.v1, f.at(600)));
    f.stack.breakMerge();
    record();
    ZARO_REQUIRE_EDIT(f, edit::makeExtract(f.project, f.on(f.v1), f.track(f.v1).clips()[0].id));
    record();
    ZARO_REQUIRE_EDIT(f, edit::makeOverwrite(f.project, f.on(f.a1), f.clip(400, 40, 1200)));
    record();

    REQUIRE(statesAfterEachEdit.size() == 20);
    REQUIRE(findViolation(f.project, f.sequence()).empty());
    const std::string finalState = statesAfterEachEdit.back();

    SECTION("undo walks back through every recorded state and ends empty") {
        for (std::size_t step = statesAfterEachEdit.size(); step-- > 0;) {
            INFO("undoing to state " << step);
            CHECK(render(f.sequence()) == statesAfterEachEdit[step]);
            REQUIRE(f.stack.undo(f.project));
        }
        CHECK(f.track(f.v1).isEmpty());
        CHECK(f.track(f.v2).isEmpty());
        CHECK(f.track(f.a1).isEmpty());
        CHECK_FALSE(f.stack.canUndo());

        SECTION("and redo walks forward through the same states") {
            for (std::size_t step = 0; step < statesAfterEachEdit.size(); ++step) {
                REQUIRE(f.stack.redo(f.project));
                INFO("redoing to state " << step);
                CHECK(render(f.sequence()) == statesAfterEachEdit[step]);
            }
            CHECK_FALSE(f.stack.canRedo());
        }
    }

    SECTION("saving and reloading reproduces the model exactly") {
        const auto text = io::saveProjectToString(f.project);
        REQUIRE(text);
        const auto loaded = io::loadProjectFromString(*text);
        REQUIRE(loaded);

        CHECK(loaded->project == f.project);
        CHECK(render(*loaded->project.findSequence(f.sequenceId)) == finalState);
        CHECK(findViolation(loaded->project, *loaded->project.findSequence(f.sequenceId)).empty());
    }
}

TEST_CASE("Random edit sequences never break the model's invariants", "[edit][fuzz]") {
    // Operations are thrown at the model in arbitrary order with arbitrary
    // arguments. Most will be refused -- that is the point: a refusal must
    // leave the model untouched, and an acceptance must leave it valid.
    constexpr int kSeeds = 40;
    constexpr int kStepsPerSeed = 150;

    for (int seed = 0; seed < kSeeds; ++seed) {
        Fixture f;
        std::mt19937 random{static_cast<std::mt19937::result_type>(seed)};

        const auto pickTrack = [&] {
            const std::array<model::TrackId, 3> tracks{f.v1, f.v2, f.a1};
            return tracks[std::uniform_int_distribution<std::size_t>{0, 2}(random)];
        };
        const auto pickFrames = [&](std::int64_t low, std::int64_t high) {
            return f.at(std::uniform_int_distribution<std::int64_t>{low, high}(random));
        };
        const auto pickClipOn = [&](model::TrackId id) -> model::ClipId {
            const auto& clips = f.track(id).clips();
            if (clips.empty()) {
                return {};
            }
            return clips[std::uniform_int_distribution<std::size_t>{0, clips.size() - 1}(random)]
                .id;
        };

        for (int step = 0; step < kStepsPerSeed; ++step) {
            const model::TrackId track = pickTrack();
            const model::ClipId clip = pickClipOn(track);
            const edit::EditTarget target = f.on(track);
            const bool rippleAll = std::uniform_int_distribution<int>{0, 3}(random) == 0;
            const Edge edge =
                std::uniform_int_distribution<int>{0, 1}(random) == 0 ? Edge::In : Edge::Out;

            const std::string before = render(f.sequence());
            Result<edit::CommandPtr> built = Error{ErrorCode::Unknown, "unset"};

            switch (std::uniform_int_distribution<int>{0, 11}(random)) {
                case 0:
                    built = edit::makeOverwrite(
                        f.project, target,
                        f.clip(pickFrames(0, 800).frames(),
                               std::uniform_int_distribution<std::int64_t>{1, 120}(random),
                               std::uniform_int_distribution<std::int64_t>{0, 3000}(random)));
                    break;
                case 1:
                    built = edit::makeInsert(
                        f.project, target,
                        f.clip(pickFrames(0, 800).frames(),
                               std::uniform_int_distribution<std::int64_t>{1, 120}(random)),
                        rippleAll);
                    break;
                case 2:
                    built = edit::makeRazor(f.project, target, pickFrames(0, 900));
                    break;
                case 3:
                    built = edit::makeLift(f.project, target, clip);
                    break;
                case 4:
                    built = edit::makeExtract(f.project, target, clip);
                    break;
                case 5:
                    built = edit::makeRippleDelete(
                        f.project, target, time::TimeRange{pickFrames(0, 800), pickFrames(1, 100)},
                        rippleAll);
                    break;
                case 6:
                    built = edit::makeTrim(f.project, target, clip, edge, pickFrames(-60, 60));
                    break;
                case 7:
                    built = edit::makeRippleTrim(f.project, target, clip, edge, pickFrames(-60, 60),
                                                 rippleAll);
                    break;
                case 8:
                    built = edit::makeRoll(f.project, target, clip, pickFrames(-60, 60));
                    break;
                case 9:
                    built = edit::makeSlip(f.project, target, clip, pickFrames(-200, 200));
                    break;
                case 10:
                    built = edit::makeSlide(f.project, target, clip, pickFrames(-60, 60));
                    break;
                default:
                    built =
                        edit::makeMove(f.project, target, clip, pickTrack(), pickFrames(0, 900));
                    break;
            }

            if (!built) {
                // A refused edit must not have touched anything.
                if (render(f.sequence()) != before) {
                    FAIL("seed " << seed << " step " << step
                                 << ": a refused edit mutated the model\nbefore:\n"
                                 << before << "after:\n"
                                 << render(f.sequence()));
                }
                continue;
            }

            f.stack.execute(f.project, std::move(*built));
            if (const std::string violation = findViolation(f.project, f.sequence());
                !violation.empty()) {
                FAIL("seed " << seed << " step " << step << ": " << violation << "\nbefore:\n"
                             << before << "after:\n"
                             << render(f.sequence()));
            }
        }

        // However far it wandered, it has to come all the way back.
        while (f.stack.undo(f.project)) {
        }
        if (const std::string violation = findViolation(f.project, f.sequence());
            !violation.empty()) {
            FAIL("seed " << seed << ": after undoing everything: " << violation);
        }
        CHECK(f.track(f.v1).isEmpty());
        CHECK(f.track(f.v2).isEmpty());
        CHECK(f.track(f.a1).isEmpty());
    }
}

TEST_CASE("Fuzzed projects survive serialization", "[edit][fuzz][io]") {
    for (int seed = 100; seed < 110; ++seed) {
        Fixture f;
        std::mt19937 random{static_cast<std::mt19937::result_type>(seed)};

        for (int step = 0; step < 60; ++step) {
            auto built = edit::makeOverwrite(
                f.project, f.on(std::uniform_int_distribution<int>{0, 1}(random) ? f.v1 : f.a1),
                f.clip(std::uniform_int_distribution<std::int64_t>{0, 600}(random),
                       std::uniform_int_distribution<std::int64_t>{1, 90}(random),
                       std::uniform_int_distribution<std::int64_t>{0, 3000}(random)));
            if (built) {
                f.stack.execute(f.project, std::move(*built));
            }
        }

        const auto text = io::saveProjectToString(f.project);
        REQUIRE(text);
        const auto loaded = io::loadProjectFromString(*text);
        REQUIRE(loaded);
        if (!(loaded->project == f.project)) {
            FAIL("seed " << seed << ": round trip changed the project\n"
                         << render(f.sequence()) << "became\n"
                         << render(*loaded->project.findSequence(f.sequenceId)));
        }
    }
}
