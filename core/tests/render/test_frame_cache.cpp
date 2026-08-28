#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/FrameCache.h"

using namespace zaro;
using model::MediaRefId;
using render::FrameCache;
using render::RgbaImage;

namespace {

const time::Rational kRate = time::rates::fps25;

time::RationalTime at(std::int64_t frame) {
    return time::RationalTime{frame, kRate};
}

/// 64x64 RGBA float is 16 KB, which makes the budget arithmetic legible.
RgbaImage frame() {
    return RgbaImage{64, 64};
}
constexpr std::size_t kFrameBytes = 64 * 64 * 4 * sizeof(float);

}  // namespace

TEST_CASE("A cached frame is found again", "[render][cache]") {
    FrameCache cache;
    const MediaRefId media{1};

    CHECK(cache.find(media, at(0)) == nullptr);
    CHECK(cache.misses() == 1);

    REQUIRE(cache.insert(media, at(0), frame()) != nullptr);
    CHECK(cache.find(media, at(0)) != nullptr);
    CHECK(cache.hits() == 1);
    CHECK(cache.count() == 1);
    CHECK(cache.byteSize() == kFrameBytes);
}

TEST_CASE("Media, frame and rate all form part of the key", "[render][cache]") {
    FrameCache cache;
    cache.insert(MediaRefId{1}, at(10), frame());

    CHECK(cache.find(MediaRefId{1}, at(10)) != nullptr);
    CHECK(cache.find(MediaRefId{2}, at(10)) == nullptr);
    CHECK(cache.find(MediaRefId{1}, at(11)) == nullptr);
    // Frame 10 at 25fps and frame 10 at 50fps are different instants of
    // different material; conflating them would serve the wrong picture.
    CHECK(cache.find(MediaRefId{1}, time::RationalTime{10, time::rates::fps50}) == nullptr);
}

TEST_CASE("The budget is enforced in bytes", "[render][cache]") {
    // Four frames' worth, so the fifth insert must evict.
    FrameCache cache{kFrameBytes * 4};
    const MediaRefId media{1};

    for (std::int64_t i = 0; i < 4; ++i) {
        cache.insert(media, at(i), frame());
    }
    CHECK(cache.count() == 4);
    CHECK(cache.byteSize() <= cache.budgetBytes());

    cache.insert(media, at(4), frame());
    CHECK(cache.count() == 4);
    CHECK(cache.byteSize() <= cache.budgetBytes());
    // Frame 0 was least recently used.
    CHECK(cache.find(media, at(0)) == nullptr);
    CHECK(cache.find(media, at(4)) != nullptr);
}

TEST_CASE("Eviction is least recently used, not oldest inserted", "[render][cache]") {
    // Scrubbing back and forth is the access pattern that matters most for
    // feel, and it is the one a first-in-first-out cache handles worst.
    FrameCache cache{kFrameBytes * 3};
    const MediaRefId media{1};

    cache.insert(media, at(0), frame());
    cache.insert(media, at(1), frame());
    cache.insert(media, at(2), frame());

    // Touch frame 0 so it is no longer the oldest use.
    REQUIRE(cache.find(media, at(0)) != nullptr);

    cache.insert(media, at(3), frame());
    CHECK(cache.find(media, at(0)) != nullptr);
    CHECK(cache.find(media, at(1)) == nullptr);
}

TEST_CASE("Re-inserting the same key replaces rather than duplicates", "[render][cache]") {
    FrameCache cache;
    const MediaRefId media{1};
    cache.insert(media, at(0), frame());
    cache.insert(media, at(0), frame());
    CHECK(cache.count() == 1);
    CHECK(cache.byteSize() == kFrameBytes);
}

TEST_CASE("A frame larger than the whole budget is not stored", "[render][cache]") {
    // Storing it would evict everything else to hold something that must itself
    // be evicted on the next insert.
    FrameCache cache{1024};
    CHECK(cache.insert(MediaRefId{1}, at(0), frame()) == nullptr);
    CHECK(cache.count() == 0);
    CHECK(cache.byteSize() == 0);
}

TEST_CASE("Lowering the budget evicts immediately", "[render][cache]") {
    FrameCache cache{kFrameBytes * 8};
    for (std::int64_t i = 0; i < 8; ++i) {
        cache.insert(MediaRefId{1}, at(i), frame());
    }
    CHECK(cache.count() == 8);

    cache.setBudgetBytes(kFrameBytes * 2);
    CHECK(cache.count() == 2);
    CHECK(cache.byteSize() <= kFrameBytes * 2);
}

TEST_CASE("Evicting one media reference leaves the others", "[render][cache]") {
    // What relinking a file, or toggling its proxy, has to do.
    FrameCache cache;
    cache.insert(MediaRefId{1}, at(0), frame());
    cache.insert(MediaRefId{1}, at(1), frame());
    cache.insert(MediaRefId{2}, at(0), frame());

    cache.evict(MediaRefId{1});
    CHECK(cache.count() == 1);
    CHECK(cache.byteSize() == kFrameBytes);
    CHECK(cache.find(MediaRefId{2}, at(0)) != nullptr);
    CHECK(cache.find(MediaRefId{1}, at(0)) == nullptr);
}

TEST_CASE("Clearing empties it", "[render][cache]") {
    FrameCache cache;
    cache.insert(MediaRefId{1}, at(0), frame());
    cache.clear();
    CHECK(cache.count() == 0);
    CHECK(cache.byteSize() == 0);
}

TEST_CASE("A linear pass over more frames than fit stays within budget", "[render][cache]") {
    // Playback: every frame is a miss, and the cache must not grow unbounded
    // while providing no benefit.
    FrameCache cache{kFrameBytes * 10};
    for (std::int64_t i = 0; i < 500; ++i) {
        if (cache.find(MediaRefId{1}, at(i)) == nullptr) {
            cache.insert(MediaRefId{1}, at(i), frame());
        }
        REQUIRE(cache.byteSize() <= cache.budgetBytes());
    }
    CHECK(cache.hits() == 0);
    CHECK(cache.misses() == 500);
    CHECK(cache.count() == 10);
}
