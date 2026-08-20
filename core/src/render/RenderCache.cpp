#include "zaro/core/render/RenderCache.h"

#include <algorithm>

#include "zaro/core/io/ProjectIo.h"
#include "zaro/core/render/RenderGraph.h"

namespace zaro::render {

namespace {

void mix(std::uint64_t& hash, std::uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
}

/// The media a clip actually reads, hashed as the file it resolves to.
///
/// Relinking a clip to a different file, or attaching a proxy to the one it
/// already has, changes the picture without touching the clip -- so the media
/// reference is part of the recipe even though it is not part of the edit.
void mixMedia(std::uint64_t& hash, const model::Project* project, model::MediaRefId id) {
    if (project == nullptr) {
        mix(hash, id.value());
        return;
    }
    if (const model::MediaRef* media = project->findMedia(id)) {
        mix(hash, io::fingerprint(*media));
    }
}

void mixClip(std::uint64_t& hash, const model::Project* project, const model::Clip& clip,
             const time::RationalTime& at, std::int32_t depth);

/// Everything one video track contributes at `at`.
///
/// Deliberately shaped like the loop in RenderGraph::compositeInto, because it
/// has to answer the same question: what is drawn here. The two are checked
/// against each other by a test that renders a frame, changes one thing, and
/// requires either both to move or neither.
void mixTrack(std::uint64_t& hash, const model::Project* project, const model::Sequence& sequence,
              const model::Track& track, const time::RationalTime& at, std::int32_t depth) {
    if (!sequence.isAudible(track)) {
        // A hidden track contributes nothing, and hiding one has to change the
        // recipe -- so the fact of it being hidden is what goes in.
        mix(hash, 0x4d757465ULL);
        return;
    }
    if (const model::Transition* transition = track.transitionAt(at)) {
        const model::Clip* outgoing = track.find(transition->from);
        const model::Clip* incoming = track.find(transition->to);
        if (outgoing != nullptr && incoming != nullptr) {
            mix(hash, io::fingerprint(*transition));
            mixClip(hash, project, *outgoing, at, depth);
            mixClip(hash, project, *incoming, at, depth);
            return;
        }
    }
    if (const model::Clip* clip = track.clipAt(at)) {
        mixClip(hash, project, *clip, at, depth);
    }
}

void mixClip(std::uint64_t& hash, const model::Project* project, const model::Clip& clip,
             const time::RationalTime& at, std::int32_t depth) {
    mix(hash, io::fingerprint(clip));
    if (clip.nested.isValid()) {
        // The same depth limit the renderer uses, for the same reason: a
        // project that arrived with a cycle in it must not take this down
        // either.
        constexpr std::int32_t kMaxDepth = 8;
        if (depth >= kMaxDepth || project == nullptr) {
            return;
        }
        const model::Sequence* inner = project->findSequence(clip.nested);
        if (inner == nullptr) {
            return;
        }
        const time::RationalTime innerAt = clip.sourceTimeAt(at);
        for (const model::Track& track : inner->videoTracks()) {
            mixTrack(hash, project, *inner, track, innerAt, depth + 1);
        }
        if (inner->captions().isBurnedIn()) {
            mix(hash, io::fingerprint(inner->captions()));
        }
        return;
    }
    mixMedia(hash, project, clip.activeSource());
}

}  // namespace

std::uint64_t frameRecipe(const model::Project* project, const model::Sequence& sequence,
                          const time::RationalTime& at) {
    std::uint64_t hash = 14695981039346656037ULL;
    mix(hash, static_cast<std::uint64_t>(sequence.width()));
    mix(hash, static_cast<std::uint64_t>(sequence.height()));
    mix(hash, static_cast<std::uint64_t>(sequence.frameRate().num()));
    mix(hash, static_cast<std::uint64_t>(sequence.frameRate().den()));
    // Not part of any clip: it decides which of two files every clip in the
    // project reads, and a frame rendered from proxies is not the frame.
    mix(hash, project != nullptr && project->usingProxies() ? 1ULL : 0ULL);

    for (const model::Track& track : sequence.videoTracks()) {
        mixTrack(hash, project, sequence, track, at, 0);
    }
    if (sequence.captions().isBurnedIn()) {
        // The whole track, not the cues showing now: a caption's *range* is
        // what decides whether it shows, so a cue moved onto this frame has to
        // change the recipe of a frame it was not previously on.
        mix(hash, io::fingerprint(sequence.captions()));
    }
    return hash;
}

std::size_t RenderCache::KeyHash::operator()(const Key& key) const noexcept {
    std::size_t hash = std::hash<std::uint64_t>{}(key.sequence);
    hash ^=
        std::hash<std::int64_t>{}(key.frames) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    hash ^=
        std::hash<std::int64_t>{}(key.rateNum) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    hash ^=
        std::hash<std::int64_t>{}(key.rateDen) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    return hash;
}

RenderCache::Entry RenderCache::find(model::SequenceId sequence, const time::RationalTime& at,
                                     std::uint64_t recipe) {
    const Key key{sequence.value(), at.frames(), at.rate().num(), at.rate().den()};
    const auto found = index_.find(key);
    if (found == index_.end()) {
        ++misses_;
        return Entry{nullptr, 0, 0};
    }
    if (found->second->recipe != recipe) {
        // Something that shows on this frame has changed. Dropped rather than
        // overwritten in place, so that the memory goes back to the budget even
        // if nothing re-renders this instant.
        ++stale_;
        byteSize_ -= found->second->image.byteSize();
        entries_.erase(found->second);
        index_.erase(found);
        return Entry{nullptr, 0, 0};
    }
    ++hits_;
    entries_.splice(entries_.begin(), entries_, found->second);
    const Record& record = entries_.front();
    return Entry{&record.image, record.clipCount, record.skippedText};
}

bool RenderCache::contains(model::SequenceId sequence, const time::RationalTime& at,
                           std::uint64_t recipe) const {
    const Key key{sequence.value(), at.frames(), at.rate().num(), at.rate().den()};
    const auto found = index_.find(key);
    return found != index_.end() && found->second->recipe == recipe;
}

void RenderCache::insert(model::SequenceId sequence, const time::RationalTime& at,
                         std::uint64_t recipe, RgbaImage frame, std::int32_t clipCount,
                         std::int32_t skippedText) {
    const Key key{sequence.value(), at.frames(), at.rate().num(), at.rate().den()};
    const std::size_t bytes = frame.byteSize();

    if (const auto existing = index_.find(key); existing != index_.end()) {
        byteSize_ -= existing->second->image.byteSize();
        entries_.erase(existing->second);
        index_.erase(existing);
    }
    if (bytes > budgetBytes_) {
        return;
    }

    entries_.push_front(Record{key, recipe, std::move(frame), clipCount, skippedText});
    index_[key] = entries_.begin();
    byteSize_ += bytes;
    evictUntilWithinBudget();
}

void RenderCache::evictUntilWithinBudget() {
    while (byteSize_ > budgetBytes_ && entries_.size() > 1) {
        const Record& oldest = entries_.back();
        byteSize_ -= oldest.image.byteSize();
        index_.erase(oldest.key);
        entries_.pop_back();
    }
}

void RenderCache::clear() {
    entries_.clear();
    index_.clear();
    byteSize_ = 0;
}

void RenderCache::evict(model::SequenceId sequence) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->key.sequence == sequence.value()) {
            byteSize_ -= it->image.byteSize();
            index_.erase(it->key);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

void RenderCache::setBudgetBytes(std::size_t bytes) {
    budgetBytes_ = bytes;
    evictUntilWithinBudget();
}

Result<PrerenderStats> prerender(RenderGraph& graph, RenderCache& cache,
                                 const model::Project* project, const model::Sequence& sequence,
                                 const time::TimeRange& range,
                                 const std::function<bool(std::int32_t, std::int32_t)>& progress) {
    PrerenderStats stats;
    if (range.isEmpty()) {
        return stats;
    }
    const time::Rational rate = sequence.frameRate();
    const time::TimeRange atRate = range.rescaledTo(rate);
    const std::int64_t first = atRate.start().frames();
    const std::int64_t total = atRate.duration().frames();
    if (total <= 0) {
        return stats;
    }

    RgbaImage frame;
    for (std::int64_t i = 0; i < total; ++i) {
        if (progress && !progress(static_cast<std::int32_t>(i), static_cast<std::int32_t>(total))) {
            stats.cancelled = true;
            return stats;
        }
        const time::RationalTime at{first + i, rate};
        const std::uint64_t recipe = frameRecipe(project, sequence, at);
        if (cache.contains(sequence.id(), at, recipe)) {
            ++stats.alreadyCached;
            continue;
        }
        if (Status status = graph.compositeInto(sequence, at, frame); !status) {
            // One frame that will not render is not a reason to abandon the
            // rest of the range: the same missing file would leave a gap during
            // playback rather than stopping it.
            continue;
        }
        cache.insert(sequence.id(), at, recipe, frame.clone(), graph.lastClipCount(),
                     graph.lastSkippedTextCount());
        ++stats.rendered;
    }
    if (progress) {
        static_cast<void>(
            progress(static_cast<std::int32_t>(total), static_cast<std::int32_t>(total)));
    }
    return stats;
}

std::vector<time::TimeRange> cachedSpans(const RenderCache& cache, const model::Project* project,
                                         const model::Sequence& sequence,
                                         const time::TimeRange& range, std::int32_t samples) {
    std::vector<time::TimeRange> spans;
    if (range.isEmpty() || samples <= 0) {
        return spans;
    }
    const time::Rational rate = sequence.frameRate();
    const time::TimeRange atRate = range.rescaledTo(rate);
    const std::int64_t first = atRate.start().frames();
    const std::int64_t total = atRate.duration().frames();
    if (total <= 0) {
        return spans;
    }
    const std::int64_t step = std::max<std::int64_t>(1, total / std::max<std::int64_t>(1, samples));

    std::int64_t runStart = -1;
    for (std::int64_t i = 0; i < total; i += step) {
        const time::RationalTime at{first + i, rate};
        const bool cached = cache.contains(sequence.id(), at, frameRecipe(project, sequence, at));
        if (cached && runStart < 0) {
            runStart = i;
        } else if (!cached && runStart >= 0) {
            spans.push_back(time::TimeRange{time::RationalTime{first + runStart, rate},
                                            time::RationalTime{i - runStart, rate}});
            runStart = -1;
        }
    }
    if (runStart >= 0) {
        spans.push_back(time::TimeRange{time::RationalTime{first + runStart, rate},
                                        time::RationalTime{total - runStart, rate}});
    }
    return spans;
}

}  // namespace zaro::render
