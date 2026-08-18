#include <catch2/catch_test_macros.hpp>

#include "zaro/core/model/Ids.h"
#include "zaro/core/model/Track.h"

using namespace zaro;
using model::Clip;
using model::ClipId;
using model::Track;
using model::TrackId;
using model::TrackKind;

namespace {

const time::Rational kRate = time::rates::fps25;

time::RationalTime at(std::int64_t frames) {
    return time::RationalTime{frames, kRate};
}

Clip makeClip(std::uint64_t id, std::int64_t start, std::int64_t duration) {
    Clip clip;
    clip.id = ClipId{id};
    clip.sourceRange = time::TimeRange{at(0), at(duration)};
    clip.timelineRange = time::TimeRange{at(start), at(duration)};
    return clip;
}

Track makeTrack() {
    return Track{TrackId{1}, TrackKind::Video, "V1"};
}

}  // namespace

TEST_CASE("Track keeps clips sorted regardless of insertion order", "[model][track]") {
    Track track = makeTrack();
    track.insert(makeClip(3, 200, 50));
    track.insert(makeClip(1, 0, 50));
    track.insert(makeClip(2, 100, 50));

    REQUIRE(track.clips().size() == 3);
    CHECK(track.clips()[0].id == ClipId{1});
    CHECK(track.clips()[1].id == ClipId{2});
    CHECK(track.clips()[2].id == ClipId{3});
}

TEST_CASE("clipAt finds the clip under a time, and nothing in a gap", "[model][track]") {
    Track track = makeTrack();
    track.insert(makeClip(1, 0, 50));
    track.insert(makeClip(2, 100, 50));

    CHECK(track.clipAt(at(0))->id == ClipId{1});
    CHECK(track.clipAt(at(49))->id == ClipId{1});
    // Half open: frame 50 belongs to the gap, not to the clip that ends there.
    CHECK(track.clipAt(at(50)) == nullptr);
    CHECK(track.clipAt(at(99)) == nullptr);
    CHECK(track.clipAt(at(100))->id == ClipId{2});
    CHECK(track.clipAt(at(149))->id == ClipId{2});
    CHECK(track.clipAt(at(150)) == nullptr);
}

TEST_CASE("Abutting clips leave no gap and no overlap", "[model][track]") {
    Track track = makeTrack();
    track.insert(makeClip(1, 0, 50));
    track.insert(makeClip(2, 50, 50));

    for (std::int64_t frame = 0; frame < 100; ++frame) {
        const Clip* found = track.clipAt(at(frame));
        REQUIRE(found != nullptr);
        CHECK(found->id == (frame < 50 ? ClipId{1} : ClipId{2}));
    }
}

TEST_CASE("isRangeFree reports occupancy, honouring the ignored clip", "[model][track]") {
    Track track = makeTrack();
    track.insert(makeClip(1, 100, 50));

    CHECK(track.isRangeFree(time::TimeRange{at(0), at(100)}));
    CHECK(track.isRangeFree(time::TimeRange{at(150), at(100)}));
    CHECK_FALSE(track.isRangeFree(time::TimeRange{at(120), at(10)}));
    CHECK_FALSE(track.isRangeFree(time::TimeRange{at(90), at(20)}));

    // Ignoring a clip is how a trim asks "would this fit if I were not here".
    CHECK(track.isRangeFree(time::TimeRange{at(90), at(80)}, ClipId{1}));
}

TEST_CASE("clipsIn returns everything intersecting a range", "[model][track]") {
    Track track = makeTrack();
    track.insert(makeClip(1, 0, 50));
    track.insert(makeClip(2, 50, 50));
    track.insert(makeClip(3, 100, 50));

    CHECK(track.clipsIn(time::TimeRange{at(0), at(150)}).size() == 3);
    CHECK(track.clipsIn(time::TimeRange{at(40), at(20)}).size() == 2);
    CHECK(track.clipsIn(time::TimeRange{at(0), at(50)}).size() == 1);
    CHECK(track.clipsIn(time::TimeRange{at(200), at(50)}).empty());
}

TEST_CASE("shiftFrom moves only clips at or after the point", "[model][track]") {
    Track track = makeTrack();
    track.insert(makeClip(1, 0, 50));
    track.insert(makeClip(2, 100, 50));
    track.insert(makeClip(3, 200, 50));

    track.shiftFrom(at(100), at(25));
    CHECK(track.clips()[0].start() == at(0));
    CHECK(track.clips()[1].start() == at(125));
    CHECK(track.clips()[2].start() == at(225));

    SECTION("and can close a gap by shifting backwards") {
        track.shiftFrom(at(125), at(-25));
        CHECK(track.clips()[1].start() == at(100));
        CHECK(track.clips()[2].start() == at(200));
    }
}

TEST_CASE("extent spans the first clip's start to the last clip's end", "[model][track]") {
    Track track = makeTrack();
    CHECK(track.extent().isEmpty());

    track.insert(makeClip(1, 25, 50));
    track.insert(makeClip(2, 200, 50));
    CHECK(track.extent().start() == at(25));
    CHECK(track.extent().endExclusive() == at(250));
}

TEST_CASE("remove and replace maintain identity and order", "[model][track]") {
    Track track = makeTrack();
    track.insert(makeClip(1, 0, 50));
    track.insert(makeClip(2, 100, 50));

    SECTION("remove returns the clip it took out") {
        const Clip removed = track.remove(ClipId{1});
        CHECK(removed.id == ClipId{1});
        CHECK(track.clips().size() == 1);
        CHECK(track.find(ClipId{1}) == nullptr);
    }

    SECTION("replace can move a clip, and it re-sorts") {
        Clip moved = makeClip(1, 300, 50);
        track.replace(ClipId{1}, moved);
        REQUIRE(track.clips().size() == 2);
        CHECK(track.clips()[0].id == ClipId{2});
        CHECK(track.clips()[1].id == ClipId{1});
        CHECK(track.clips()[1].start() == at(300));
    }
}
