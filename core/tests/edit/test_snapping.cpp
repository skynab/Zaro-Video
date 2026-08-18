#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/edit/Snapping.h"

#include "ModelFixtures.h"

using namespace zaro;
using zaro::testing::Fixture;

namespace {

Fixture withClips() {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(100, 50))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(300, 50))));
    return f;
}

}  // namespace

TEST_CASE("Snapping latches onto edit points within the threshold", "[edit][snap]") {
    Fixture f = withClips();
    const time::RationalTime threshold = f.at(5);

    SECTION("a clip start") {
        const auto result = edit::snapTime(f.sequence(), f.at(103), threshold);
        CHECK(result.snapped());
        CHECK(result.time == f.at(100));
        CHECK(result.kind == edit::SnapKind::ClipStart);
        CHECK(result.track == f.v1);
    }

    SECTION("a clip end") {
        const auto result = edit::snapTime(f.sequence(), f.at(148), threshold);
        CHECK(result.time == f.at(150));
        CHECK(result.kind == edit::SnapKind::ClipEnd);
    }

    SECTION("the start of the sequence") {
        const auto result = edit::snapTime(f.sequence(), f.at(3), threshold);
        CHECK(result.time == f.at(0));
        CHECK(result.kind == edit::SnapKind::SequenceStart);
    }

    SECTION("across tracks, because sync matters more than proximity") {
        const auto result = edit::snapTime(f.sequence(), f.at(298), threshold);
        CHECK(result.time == f.at(300));
        CHECK(result.track == f.a1);
    }
}

TEST_CASE("Nothing in range means no snap", "[edit][snap]") {
    Fixture f = withClips();
    const auto result = edit::snapTime(f.sequence(), f.at(200), f.at(5));
    CHECK_FALSE(result.snapped());
    CHECK(result.time == f.at(200));
    CHECK(result.kind == edit::SnapKind::None);
}

TEST_CASE("The nearest candidate wins", "[edit][snap]") {
    Fixture f = withClips();
    // 140 is 40 from the start and 10 from the end, with a threshold that
    // reaches both.
    const auto result = edit::snapTime(f.sequence(), f.at(140), f.at(50));
    CHECK(result.time == f.at(150));
}

TEST_CASE("A dragged clip does not snap to itself", "[edit][snap]") {
    Fixture f = withClips();
    const model::ClipId dragged = f.track(f.v1).clips()[0].id;

    const auto without = edit::snapTime(f.sequence(), f.at(102), f.at(5), dragged);
    CHECK_FALSE(without.snapped());

    const auto with = edit::snapTime(f.sequence(), f.at(102), f.at(5));
    CHECK(with.snapped());
}

TEST_CASE("The playhead is a snap target when supplied", "[edit][snap]") {
    Fixture f = withClips();
    const time::RationalTime playhead = f.at(220);
    const auto result = edit::snapTime(f.sequence(), f.at(222), f.at(5), {}, &playhead);
    CHECK(result.time == playhead);
    CHECK(result.kind == edit::SnapKind::Playhead);
}

TEST_CASE("Candidates are sorted and deduplicated", "[edit][snap]") {
    Fixture f;
    // Two abutting clips share an edit point; it should be listed once.
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50))));

    const auto candidates = edit::snapCandidates(f.sequence());
    CHECK(candidates.size() == 3);  // 0, 50, 100
    CHECK(std::is_sorted(candidates.begin(), candidates.end()));
}
