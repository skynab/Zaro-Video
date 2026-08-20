#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <unordered_map>
#include <vector>

#include "zaro/core/Error.h"
#include "zaro/core/model/Project.h"
#include "zaro/core/model/Sequence.h"
#include "zaro/core/render/RgbaImage.h"
#include "zaro/core/time/TimeRange.h"

namespace zaro::render {

class RenderGraph;

/// A hash of everything that decides what a sequence looks like at one instant.
///
/// This is the whole invalidation strategy. Rather than track which cached
/// frames an edit dirtied -- a dependency graph that has to be right for every
/// operation, including the ones added next year -- a cached frame carries the
/// recipe it was made from, and a lookup that finds a different recipe is a
/// miss. Nothing has to be told that anything changed.
///
/// It is exact in both directions, which matters more than it sounds. A scheme
/// that over-invalidates throws away work and people stop trusting the cache;
/// one that under-invalidates shows a frame from before the change, and the
/// person looking at it has no reason to suspect the picture rather than their
/// own edit.
///
/// Only what is *drawn* at `at` goes in. Moving a clip somewhere else on the
/// timeline changes nothing here, so the frames that clip does not touch stay
/// cached -- which is the point of caching a timeline rather than a project.
///
/// `project` may be null; a nested clip and a proxy toggle both need it, and a
/// headless graph that was never given one is a graph where neither applies.
[[nodiscard]] std::uint64_t frameRecipe(const model::Project* project,
                                        const model::Sequence& sequence,
                                        const time::RationalTime& at);

/// Composited sequence frames, kept so that playing a range a second time does
/// not composite it a second time.
///
/// Distinct from FrameCache, which holds *decoded* frames: that one saves a
/// decode, this one saves the whole graph -- every grade, mask, transition and
/// nested sequence above it. On a heavy stack the decode is the cheap part.
///
/// In memory, not on disk. A disk cache buys survival across sessions and
/// costs an encode, a codec choice, a directory to manage and a garbage
/// collection policy; none of that is needed to make a graded stack play back,
/// which is the problem in front of us.
class RenderCache {
public:
    /// Bigger than FrameCache's, because these frames are much more expensive
    /// to recreate and there is exactly one per timeline instant rather than
    /// one per clip per instant.
    static constexpr std::size_t kDefaultBudgetBytes = 1024u * 1024u * 1024u;

    explicit RenderCache(std::size_t budgetBytes = kDefaultBudgetBytes)
        : budgetBytes_{budgetBytes} {}

    /// What was rendered, and what it cost to render -- so a frame served from
    /// here reports the same diagnostics as one that was just composited.
    struct Entry {
        const RgbaImage* image;
        std::int32_t clipCount;
        std::int32_t skippedText;
    };

    /// The cached frame, or null. A frame stored under a different recipe is
    /// stale: it is dropped here rather than left to age out, so that a stale
    /// entry stops counting as cached the moment anyone asks about it.
    [[nodiscard]] Entry find(model::SequenceId sequence, const time::RationalTime& at,
                             std::uint64_t recipe);

    /// Whether a current frame is stored, without disturbing the LRU order.
    /// For drawing a cache bar: asking about a thousand frames must not
    /// reorder the thousand frames somebody is about to play.
    [[nodiscard]] bool contains(model::SequenceId sequence, const time::RationalTime& at,
                                std::uint64_t recipe) const;

    void insert(model::SequenceId sequence, const time::RationalTime& at, std::uint64_t recipe,
                RgbaImage frame, std::int32_t clipCount, std::int32_t skippedText);

    void clear();
    /// Drop everything belonging to one sequence, for when it is closed.
    void evict(model::SequenceId sequence);

    [[nodiscard]] std::size_t byteSize() const noexcept { return byteSize_; }
    [[nodiscard]] std::size_t budgetBytes() const noexcept { return budgetBytes_; }
    [[nodiscard]] std::size_t count() const noexcept { return entries_.size(); }
    void setBudgetBytes(std::size_t bytes);

    [[nodiscard]] std::uint64_t hits() const noexcept { return hits_; }
    [[nodiscard]] std::uint64_t misses() const noexcept { return misses_; }
    /// Frames found but thrown away because the recipe had moved on. Worth
    /// showing separately from a miss: a high number means the cache is being
    /// invalidated faster than it is being filled.
    [[nodiscard]] std::uint64_t stale() const noexcept { return stale_; }
    void resetStatistics() noexcept { hits_ = misses_ = stale_ = 0; }

private:
    struct Key {
        std::uint64_t sequence;
        std::int64_t frames;
        std::int64_t rateNum;
        std::int64_t rateDen;

        friend bool operator==(const Key&, const Key&) = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept;
    };
    struct Record {
        Key key;
        std::uint64_t recipe;
        RgbaImage image;
        std::int32_t clipCount;
        std::int32_t skippedText;
    };

    void evictUntilWithinBudget();

    /// Most recently used at the front.
    std::list<Record> entries_;
    std::unordered_map<Key, std::list<Record>::iterator, KeyHash> index_;
    std::size_t byteSize_{0};
    std::size_t budgetBytes_;
    std::uint64_t hits_{0};
    std::uint64_t misses_{0};
    std::uint64_t stale_{0};
};

/// Render a range into the cache ahead of playing it.
///
/// This is what makes the cache worth having. Memoisation alone only helps the
/// *second* pass over a range, and the first pass is the one somebody is
/// sitting through. Rendering ahead turns "unplayable" into "playable after a
/// wait you can watch finish".
struct PrerenderStats {
    std::int32_t rendered{0};
    std::int32_t alreadyCached{0};
    bool cancelled{false};
};

/// `progress` is called with (done, total) and returns false to stop. Frames
/// already rendered stay in the cache: a cancelled render is a partial one,
/// not a wasted one.
[[nodiscard]] Result<PrerenderStats> prerender(
    RenderGraph& graph, RenderCache& cache, const model::Project* project,
    const model::Sequence& sequence, const time::TimeRange& range,
    const std::function<bool(std::int32_t, std::int32_t)>& progress = {});

/// The parts of `range` that are cached and current, for drawing a cache bar.
///
/// Sampled, not exhaustive: `samples` points are checked and consecutive hits
/// are merged into a span. A bar is drawn a pixel at a time and read at a
/// glance, so checking more instants than the bar has pixels buys nothing and
/// costs a hash per frame over the whole timeline on every repaint.
[[nodiscard]] std::vector<time::TimeRange> cachedSpans(const RenderCache& cache,
                                                       const model::Project* project,
                                                       const model::Sequence& sequence,
                                                       const time::TimeRange& range,
                                                       std::int32_t samples);

}  // namespace zaro::render
