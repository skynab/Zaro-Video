#include "zaro/core/model/Track.h"

#include <algorithm>

#include "zaro/core/Check.h"

namespace zaro::model {

const char* toString(TrackKind kind) noexcept {
    return kind == TrackKind::Video ? "video" : "audio";
}

namespace {

bool startsBefore(const Clip& clip, const time::RationalTime& t) {
    return clip.start() < t;
}

}  // namespace

const Clip* Track::clipAt(const time::RationalTime& t) const {
    // The last clip starting at or before t is the only candidate, because
    // nothing overlaps it.
    //
    // upper_bound, not lower_bound: lower_bound stops *at* a clip starting
    // exactly on t, and stepping back from there lands on the previous clip --
    // which would make every clip invisible on its own first frame.
    const auto it = std::upper_bound(
        clips_.begin(), clips_.end(), t,
        [](const time::RationalTime& value, const Clip& clip) { return value < clip.start(); });
    if (it == clips_.begin()) {
        return nullptr;
    }
    const Clip& candidate = *std::prev(it);
    return candidate.timelineRange.contains(t) ? &candidate : nullptr;
}

const Transition* Track::transitionAt(const time::RationalTime& t) const {
    for (const Transition& transition : transitions_) {
        if (transition.range.contains(t)) {
            return &transition;
        }
    }
    return nullptr;
}

const Transition* Track::findTransition(TransitionId id) const {
    for (const Transition& transition : transitions_) {
        if (transition.id == id) {
            return &transition;
        }
    }
    return nullptr;
}

void Track::setTransitions(std::vector<Transition> transitions) {
    transitions_ = std::move(transitions);
}

std::optional<std::size_t> Track::indexOf(ClipId id) const {
    for (std::size_t i = 0; i < clips_.size(); ++i) {
        if (clips_[i].id == id) {
            return i;
        }
    }
    return std::nullopt;
}

const Clip* Track::find(ClipId id) const {
    const auto index = indexOf(id);
    return index ? &clips_[*index] : nullptr;
}

Clip* Track::find(ClipId id) {
    const auto index = indexOf(id);
    return index ? &clips_[*index] : nullptr;
}

std::vector<const Clip*> Track::clipsIn(const time::TimeRange& range) const {
    std::vector<const Clip*> out;
    for (const Clip& clip : clips_) {
        if (clip.start() >= range.endExclusive()) {
            break;
        }
        if (clip.timelineRange.overlaps(range)) {
            out.push_back(&clip);
        }
    }
    return out;
}

std::optional<std::size_t> Track::firstIndexAtOrAfter(const time::RationalTime& t) const {
    const auto it = std::lower_bound(clips_.begin(), clips_.end(), t, startsBefore);
    if (it == clips_.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(clips_.begin(), it));
}

time::TimeRange Track::extent() const {
    if (clips_.empty()) {
        return {};
    }
    return time::TimeRange::fromStartEnd(clips_.front().start(), clips_.back().endExclusive());
}

bool Track::isRangeFree(const time::TimeRange& range, ClipId ignoring) const {
    for (const Clip& clip : clips_) {
        if (clip.id == ignoring) {
            continue;
        }
        if (clip.start() >= range.endExclusive()) {
            break;
        }
        if (clip.timelineRange.overlaps(range)) {
            return false;
        }
    }
    return true;
}

void Track::insert(Clip clip) {
    ZARO_CHECK(clip.id.isValid(), "inserting a clip with no id");
    ZARO_CHECK(!clip.timelineRange.isEmpty(), "inserting a zero-length clip");
    ZARO_CHECK(isRangeFree(clip.timelineRange),
               "inserting a clip over occupied range; clear it first");
    ZARO_CHECK(find(clip.id) == nullptr, "inserting a clip id that is already on this track");

    const auto at = std::lower_bound(clips_.begin(), clips_.end(), clip.start(), startsBefore);
    clips_.insert(at, std::move(clip));
}

Clip Track::remove(ClipId id) {
    const auto index = indexOf(id);
    ZARO_CHECK(index.has_value(), "removing a clip that is not on this track");
    Clip removed = std::move(clips_[*index]);
    clips_.erase(clips_.begin() + static_cast<std::ptrdiff_t>(*index));
    return removed;
}

void Track::replace(ClipId id, Clip clip) {
    const auto index = indexOf(id);
    ZARO_CHECK(index.has_value(), "replacing a clip that is not on this track");
    ZARO_CHECK(!clip.timelineRange.isEmpty(), "replacing with a zero-length clip");
    ZARO_CHECK(isRangeFree(clip.timelineRange, id), "replacement would overlap a neighbour");

    clips_.erase(clips_.begin() + static_cast<std::ptrdiff_t>(*index));
    const auto at = std::lower_bound(clips_.begin(), clips_.end(), clip.start(), startsBefore);
    clips_.insert(at, std::move(clip));
}

bool Track::canShiftFrom(const time::RationalTime& from, const time::RationalTime& delta) const {
    if (delta.isZero()) {
        return true;
    }
    const Clip* lastFixed = nullptr;
    for (const Clip& clip : clips_) {
        if (clip.start() < from) {
            lastFixed = &clip;
            continue;
        }
        const time::RationalTime newStart = clip.start() + delta;
        if (newStart.frames() < 0) {
            return false;
        }
        if (lastFixed != nullptr && newStart < lastFixed->endExclusive()) {
            return false;
        }
        // Shifted clips keep their spacing relative to each other, so only the
        // boundary with the unshifted part can be violated.
        break;
    }
    return true;
}

void Track::shiftFrom(const time::RationalTime& from, const time::RationalTime& delta) {
    if (delta.isZero()) {
        return;
    }
    for (Clip& clip : clips_) {
        if (clip.start() >= from) {
            clip.timelineRange =
                time::TimeRange{clip.start() + delta, clip.timelineRange.duration()};
        }
    }
    // Shifting preserves order and spacing among the shifted clips, but a
    // negative delta can drive them into the ones before `from`.
    checkInvariants();
}

void Track::setClips(std::vector<Clip> clips) {
    clips_ = std::move(clips);
    checkInvariants();
}

void Track::checkInvariants() const {
    for (std::size_t i = 0; i < clips_.size(); ++i) {
        ZARO_CHECK(!clips_[i].timelineRange.isEmpty(), "track holds a zero-length clip");
        if (i > 0) {
            ZARO_CHECK(clips_[i - 1].endExclusive() <= clips_[i].start(),
                       "track clips overlap or are out of order");
        }
    }
}

}  // namespace zaro::model
