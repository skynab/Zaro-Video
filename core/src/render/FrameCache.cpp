#include "zaro/core/render/FrameCache.h"

#include <cstdint>
#include <functional>

namespace zaro::render {

std::size_t FrameCache::KeyHash::operator()(const Key& key) const noexcept {
    // Frames within one media reference are the common neighbours, so the frame
    // number gets the low bits and the media id is folded in above it.
    std::size_t hash = std::hash<std::uint64_t>{}(key.media);
    hash ^=
        std::hash<std::int64_t>{}(key.frames) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    hash ^=
        std::hash<std::int64_t>{}(key.rateNum) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    hash ^=
        std::hash<std::int64_t>{}(key.rateDen) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    return hash;
}

const RgbaImage* FrameCache::find(model::MediaRefId media, const time::RationalTime& at) {
    const Key key{media.value(), at.frames(), at.rate().num(), at.rate().den()};
    const auto found = index_.find(key);
    if (found == index_.end()) {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    entries_.splice(entries_.begin(), entries_, found->second);
    return &entries_.front().image;
}

const RgbaImage* FrameCache::insert(model::MediaRefId media, const time::RationalTime& at,
                                    RgbaImage frame) {
    const Key key{media.value(), at.frames(), at.rate().num(), at.rate().den()};
    const std::size_t bytes = frame.byteSize();

    if (const auto existing = index_.find(key); existing != index_.end()) {
        byteSize_ -= existing->second->image.byteSize();
        entries_.erase(existing->second);
        index_.erase(existing);
    }
    if (bytes > budgetBytes_) {
        // Larger than the entire budget. Storing it would evict everything else
        // to hold something that must be evicted on the very next insert.
        return nullptr;
    }

    entries_.push_front(Entry{key, std::move(frame)});
    index_[key] = entries_.begin();
    byteSize_ += bytes;
    evictUntilWithinBudget();
    return &entries_.front().image;
}

void FrameCache::evictUntilWithinBudget() {
    while (byteSize_ > budgetBytes_ && entries_.size() > 1) {
        const Entry& oldest = entries_.back();
        byteSize_ -= oldest.image.byteSize();
        index_.erase(oldest.key);
        entries_.pop_back();
    }
}

void FrameCache::setBudgetBytes(std::size_t bytes) {
    budgetBytes_ = bytes;
    evictUntilWithinBudget();
}

void FrameCache::clear() {
    entries_.clear();
    index_.clear();
    byteSize_ = 0;
}

void FrameCache::evict(model::MediaRefId media) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->key.media == media.value()) {
            byteSize_ -= it->image.byteSize();
            index_.erase(it->key);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace zaro::render
