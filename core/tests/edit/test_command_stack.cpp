#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/edit/Operations.h"

#include "ModelFixtures.h"

using namespace zaro;
using edit::Edge;
using zaro::testing::Fixture;

TEST_CASE("Undo and redo walk the history", "[edit][undo]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(100, 50, 600))));
    const std::string full = f.layout(f.v1);

    REQUIRE(f.stack.undo(f.project));
    CHECK(f.layout(f.v1) == "0-50@500");
    REQUIRE(f.stack.undo(f.project));
    CHECK(f.layout(f.v1).empty());

    CHECK_FALSE(f.stack.canUndo());
    CHECK_FALSE(f.stack.undo(f.project));

    REQUIRE(f.stack.redo(f.project));
    REQUIRE(f.stack.redo(f.project));
    CHECK(f.layout(f.v1) == full);
    CHECK_FALSE(f.stack.redo(f.project));
}

TEST_CASE("A new edit discards the redo branch", "[edit][undo]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(100, 50))));
    REQUIRE(f.stack.undo(f.project));
    REQUIRE(f.stack.canRedo());

    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(200, 50))));
    CHECK_FALSE(f.stack.canRedo());
    CHECK(f.stack.depth() == 2);
}

TEST_CASE("Descriptions label what undo and redo will do", "[edit][undo]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    REQUIRE(f.run(edit::makeRazor(f.project, f.on(f.v1), f.at(20))));

    CHECK(f.stack.undoDescription() == "Razor");
    CHECK(f.stack.redoDescription().empty());

    REQUIRE(f.stack.undo(f.project));
    CHECK(f.stack.undoDescription() == "Overwrite");
    CHECK(f.stack.redoDescription() == "Razor");

    CHECK(f.stack.history() == std::vector<std::string>{"Razor", "Overwrite"});
}

TEST_CASE("A drag collapses into one undo step", "[edit][undo][merge]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    const model::ClipId id = f.track(f.v1).clips()[0].id;
    const std::size_t before = f.stack.depth();

    // What dragging a clip across the timeline actually emits.
    for (std::int64_t frame = 1; frame <= 100; ++frame) {
        REQUIRE(f.run(edit::makeMove(f.project, f.on(f.v1), id, f.v1, f.at(frame))));
    }
    CHECK(f.layout(f.v1) == "100-150@500");
    CHECK(f.stack.depth() == before + 1);

    SECTION("and one undo puts it back where it started") {
        REQUIRE(f.stack.undo(f.project));
        CHECK(f.layout(f.v1) == "0-50@500");
    }

    SECTION("redo returns it to the end of the drag, not the middle") {
        REQUIRE(f.stack.undo(f.project));
        REQUIRE(f.stack.redo(f.project));
        CHECK(f.layout(f.v1) == "100-150@500");
    }
}

TEST_CASE("Breaking the merge group starts a new undo step", "[edit][undo][merge]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const model::ClipId id = f.track(f.v1).clips()[0].id;

    REQUIRE(f.run(edit::makeMove(f.project, f.on(f.v1), id, f.v1, f.at(10))));
    f.stack.breakMerge();
    REQUIRE(f.run(edit::makeMove(f.project, f.on(f.v1), id, f.v1, f.at(20))));
    CHECK(f.stack.depth() == 3);
}

TEST_CASE("Different clips do not merge with each other", "[edit][undo][merge]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(100, 50))));
    const model::ClipId a = f.track(f.v1).clips()[0].id;
    const model::ClipId b = f.track(f.v1).clips()[1].id;

    REQUIRE(f.run(edit::makeMove(f.project, f.on(f.v1), a, f.v1, f.at(200))));
    REQUIRE(f.run(edit::makeMove(f.project, f.on(f.v1), b, f.v1, f.at(300))));
    CHECK(f.stack.depth() == 4);
}

TEST_CASE("A group makes several clips one undo step", "[edit][undo][merge]") {
    // What merge keys cannot do, and should not: they name *what* is being
    // changed, so two clips never merge. Setting the opacity of five selected
    // clips is five keys and one gesture, and undoing it a clip at a time is
    // not what anybody meant by it.
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(100, 50))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(200, 50))));
    const model::ClipId a = f.track(f.v1).clips()[0].id;
    const model::ClipId b = f.track(f.v1).clips()[1].id;
    const model::ClipId c = f.track(f.v1).clips()[2].id;
    const std::size_t before = f.stack.depth();

    {
        edit::CommandStack::Group group{f.stack};
        for (const model::ClipId id : {a, b, c}) {
            REQUIRE(f.run(edit::makeSetClipAudio(f.project, f.on(f.v1), id, -6.0, 0.0)));
        }
    }
    CHECK(f.stack.depth() == before + 1);
    for (const model::ClipId id : {a, b, c}) {
        CHECK(f.track(f.v1).find(id)->gainDb == -6.0);
    }

    // And one undo takes all three back, which is the point.
    REQUIRE(f.stack.undo(f.project));
    for (const model::ClipId id : {a, b, c}) {
        CHECK(f.track(f.v1).find(id)->gainDb == 0.0);
    }
    CHECK(f.stack.depth() == before + 1);

    // Redo restores all three too: the step describes the whole gesture in
    // both directions, not a replay of the first command in it.
    REQUIRE(f.stack.redo(f.project));
    for (const model::ClipId id : {a, b, c}) {
        CHECK(f.track(f.v1).find(id)->gainDb == -6.0);
    }
}

TEST_CASE("A group does not swallow what came before or after it", "[edit][undo][merge]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const model::ClipId a = f.track(f.v1).clips()[0].id;
    REQUIRE(f.run(edit::makeSetClipAudio(f.project, f.on(f.v1), a, -3.0, 0.0)));
    const std::size_t before = f.stack.depth();

    {
        edit::CommandStack::Group group{f.stack};
        REQUIRE(f.run(edit::makeSetClipAudio(f.project, f.on(f.v1), a, -6.0, 0.0)));
    }
    // A step of its own, even though its key matches the one before it: a
    // group is a gesture, and the one before it was another.
    CHECK(f.stack.depth() == before + 1);

    REQUIRE(f.run(edit::makeSetClipAudio(f.project, f.on(f.v1), a, -9.0, 0.0)));
    CHECK(f.stack.depth() == before + 2);

    REQUIRE(f.stack.undo(f.project));
    CHECK(f.track(f.v1).find(a)->gainDb == -6.0);
    REQUIRE(f.stack.undo(f.project));
    CHECK(f.track(f.v1).find(a)->gainDb == -3.0);
}

TEST_CASE("Nested groups close once, not twice", "[edit][undo][merge]") {
    // An operation that groups internally, called from a panel that is
    // grouping too. Only the outermost may close the step; if the inner one
    // did, the rest of the gesture would land in a second undo entry.
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(100, 50))));
    const model::ClipId a = f.track(f.v1).clips()[0].id;
    const model::ClipId b = f.track(f.v1).clips()[1].id;
    const std::size_t before = f.stack.depth();

    {
        edit::CommandStack::Group outer{f.stack};
        {
            edit::CommandStack::Group inner{f.stack};
            REQUIRE(f.run(edit::makeSetClipAudio(f.project, f.on(f.v1), a, -6.0, 0.0)));
        }
        REQUIRE(f.run(edit::makeSetClipAudio(f.project, f.on(f.v1), b, -6.0, 0.0)));
    }
    CHECK(f.stack.depth() == before + 1);
}

TEST_CASE("A group that executes nothing leaves no step", "[edit][undo][merge]") {
    // An operation refused inside a group contributes no command, and an empty
    // group is not an undo entry somebody has to step over.
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const std::size_t before = f.stack.depth();
    {
        edit::CommandStack::Group group{f.stack};
    }
    CHECK(f.stack.depth() == before);
}

TEST_CASE("History depth is bounded", "[edit][undo]") {
    Fixture f;
    edit::CommandStack shallow{3};

    for (std::int64_t i = 0; i < 10; ++i) {
        auto built = edit::makeOverwrite(f.project, f.on(f.v1), f.clip(i * 100, 50));
        REQUIRE(built);
        shallow.execute(f.project, std::move(*built));
        shallow.breakMerge();
    }
    CHECK(shallow.depth() == 3);

    // Only the retained history is undoable; the rest of the edits stay.
    while (shallow.undo(f.project)) {
    }
    CHECK(f.track(f.v1).clips().size() == 7);
}

TEST_CASE("Snapshot cost is proportional to the sequence, not the history", "[edit][undo]") {
    Fixture f;
    for (std::int64_t i = 0; i < 20; ++i) {
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(i * 100, 50))));
    }
    // Twenty commands over a growing sequence: the bound that matters is that
    // this stays kilobytes, not megabytes, for a timeline of this size.
    //
    // Stated per clip rather than as a total, because the total legitimately
    // grows every time a clip gains a field -- colour correction, tone curves
    // and a secondary qualifier between them added about a tenth to it, and a
    // flat cap would have to be argued upwards each time without anyone
    // deciding whether the growth was reasonable. Snapshots hold whole
    // sequences (ADR-004), so the number to watch is what one clip costs in one
    // snapshot.
    const std::size_t clips = f.track(f.v1).clips().size();
    const std::size_t snapshots = f.stack.depth();
    REQUIRE(clips > 0);
    REQUIRE(snapshots > 0);
    const std::size_t perClip = f.stack.snapshotBytes() / (clips * snapshots);
    INFO("snapshot bytes: " << f.stack.snapshotBytes() << " over " << snapshots
                            << " snapshots of up to " << clips << " clips -- " << perClip
                            << " per clip per snapshot");
    // Stated as a multiple of the model's own size rather than as a byte
    // count. sizeof(Clip) is not the same number on every standard library --
    // libstdc++'s std::string is a third larger than libc++'s, and a Clip
    // holds several -- so a fixed budget calibrated on one of them fails on
    // the other for a reason that has nothing to do with what this test is
    // about. It was 1200 against a clip of 1160 bytes: forty bytes of headroom
    // that the first wider std::string spent.
    //
    // Twice the model leaves room for the container overheads without leaving
    // room for a copy nobody meant to make. The absolute figure that used to
    // follow said the same thing a second way -- perClip is the total divided
    // by exactly these two counts -- so it is gone rather than restated.
    CHECK(perClip < 2 * sizeof(model::Clip));
}
