#pragma once

#include <cstdint>
#include <list>
#include <unordered_map>

#include "zaro/core/model/Ids.h"
#include "zaro/core/render/RgbaImage.h"
#include "zaro/core/time/RationalTime.h"

namespace zaro::render {

/// A least-recently-used cache of decoded frames, bounded by bytes.
///
/// The budget is in bytes rather than entries because that is the quantity that
/// actually runs out: a working-space frame is 8 MB at 1080p and 33 MB at 4K,
/// so "keep 100 frames" means 800 MB on one project and 3.3 GB on another. A
/// cache sized in entries is a cache that works until someone opens UHD
/// footage.
///
/// Eviction is strict LRU. Scrubbing back and forth over a few seconds -- the
/// access pattern that matters most for feel -- keeps exactly the frames being
/// revisited, and a linear playback pass evicts in the order it read.
class FrameCache {
public:
    static constexpr std::size_t kDefaultBudgetBytes = 512u * 1024u * 1024u;

    explicit FrameCache(std::size_t budgetBytes = kDefaultBudgetBytes)
        : budgetBytes_{budgetBytes} {}

    /// The cached frame, or nullptr. Marks it most recently used.
    [[nodiscard]] const RgbaImage* find(model::MediaRefId media, const time::RationalTime& at);

    /// Take ownership of a frame. A frame larger than the whole budget is
    /// simply not stored -- refusing beats evicting everything else to make
    /// room for something that will be evicted next.
    const RgbaImage* insert(model::MediaRefId media, const time::RationalTime& at, RgbaImage frame);

    void clear();
    /// Drop everything belonging to one media reference, for when it is
    /// relinked or its proxy is toggled.
    void evict(model::MediaRefId media);

    [[nodiscard]] std::size_t byteSize() const noexcept { return byteSize_; }
    [[nodiscard]] std::size_t budgetBytes() const noexcept { return budgetBytes_; }
    [[nodiscard]] std::size_t count() const noexcept { return entries_.size(); }

    void setBudgetBytes(std::size_t bytes);

    /// Hit and miss counts, for showing whether the budget is doing any good.
    [[nodiscard]] std::uint64_t hits() const noexcept { return hits_; }
    [[nodiscard]] std::uint64_t misses() const noexcept { return misses_; }
    void resetStatistics() noexcept { hits_ = misses_ = 0; }

private:
    struct Key {
        std::uint64_t media;
        std::int64_t frames;
        std::int64_t rateNum;
        std::int64_t rateDen;

        friend bool operator==(const Key&, const Key&) = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept;
    };
    struct Entry {
        Key key;
        RgbaImage image;
    };

    void evictUntilWithinBudget();

    /// Most recently used at the front.
    std::list<Entry> entries_;
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> index_;
    std::size_t byteSize_{0};
    std::size_t budgetBytes_;
    std::uint64_t hits_{0};
    std::uint64_t misses_{0};
};

}  // namespace zaro::render
