#pragma once

#include <optional>
#include <string>
#include <vector>

#include "zaro/core/model/Clip.h"
#include "zaro/core/model/Transition.h"

namespace zaro::model {

enum class TrackKind { Video, Audio };

[[nodiscard]] const char* toString(TrackKind kind) noexcept;

/// An ordered lane of non-overlapping clips.
///
/// Two invariants hold at all times, and every mutator asserts them:
///
///   * clips are sorted by timeline start
///   * no two clips overlap
///
/// Gaps are implicit -- the space between one clip's exclusive end and the
/// next clip's start. The alternative, storing explicit gap items, makes ripple
/// operations a list splice but makes the far more common question "what is
/// playing at time T" a linear walk instead of a binary search. The compositor
/// asks that question for every track on every frame, so the sparse form wins.
class Track {
public:
    Track() = default;
    Track(TrackId id, TrackKind kind, std::string name)
        : id_{id}, kind_{kind}, name_{std::move(name)} {}

    [[nodiscard]] TrackId id() const noexcept { return id_; }
    [[nodiscard]] TrackKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void setName(std::string value) { name_ = std::move(value); }

    [[nodiscard]] bool isMuted() const noexcept { return muted_; }
    void setMuted(bool value) noexcept { muted_ = value; }
    [[nodiscard]] bool isLocked() const noexcept { return locked_; }
    void setLocked(bool value) noexcept { locked_ = value; }

    /// Whether this track follows ripple edits made on other tracks.
    ///
    /// Distinct from being locked. A locked track refuses all editing; a track
    /// with sync lock off can still be edited directly but stays put when
    /// something else ripples -- which is how a music bed or a lower third is
    /// kept from sliding every time the picture is trimmed.
    [[nodiscard]] bool isSyncLocked() const noexcept { return syncLocked_; }
    void setSyncLocked(bool value) noexcept { syncLocked_ = value; }

    /// Track gain in decibels and pan from -1 to +1, applied after clip gain.
    [[nodiscard]] double gainDb() const noexcept { return gainDb_; }
    void setGainDb(double value) noexcept { gainDb_ = value; }
    [[nodiscard]] double pan() const noexcept { return pan_; }
    void setPan(double value) noexcept { pan_ = value; }

    [[nodiscard]] const std::vector<Clip>& clips() const noexcept { return clips_; }
    [[nodiscard]] const std::vector<Transition>& transitions() const noexcept {
        return transitions_;
    }

    /// The transition covering `t`, if any.
    [[nodiscard]] const Transition* transitionAt(const time::RationalTime& t) const;
    [[nodiscard]] const Transition* findTransition(TransitionId id) const;

    void setTransitions(std::vector<Transition> transitions);
    [[nodiscard]] bool isEmpty() const noexcept { return clips_.empty(); }

    /// The clip playing at `t`, or nullptr in a gap. Binary search.
    [[nodiscard]] const Clip* clipAt(const time::RationalTime& t) const;

    [[nodiscard]] const Clip* find(ClipId id) const;
    [[nodiscard]] Clip* find(ClipId id);
    [[nodiscard]] std::optional<std::size_t> indexOf(ClipId id) const;

    /// Every clip whose timeline range intersects `range`, in order.
    [[nodiscard]] std::vector<const Clip*> clipsIn(const time::TimeRange& range) const;

    /// First clip starting at or after `t`.
    [[nodiscard]] std::optional<std::size_t> firstIndexAtOrAfter(const time::RationalTime& t) const;

    /// Start of the first clip to end of the last. Empty for an empty track.
    [[nodiscard]] time::TimeRange extent() const;

    /// True when `range` is free of clips, ignoring `ignoring` if given.
    [[nodiscard]] bool isRangeFree(const time::TimeRange& range, ClipId ignoring = ClipId{}) const;

    // --- Mutators. Each keeps the sorted, non-overlapping invariant, and trips
    // --- a check rather than silently producing a corrupt track.

    /// Insert into free space. The caller must have cleared the range first;
    /// this refuses to overwrite rather than choosing a victim for you.
    void insert(Clip clip);

    Clip remove(ClipId id);

    /// Replace a clip in place. The new extent must still be free.
    void replace(ClipId id, Clip clip);

    /// Whether shiftFrom would leave the track valid. A negative delta can
    /// drive the shifted clips into the ones before `from`, and an operation
    /// that would do that has to be refused, not asserted on.
    [[nodiscard]] bool canShiftFrom(const time::RationalTime& from,
                                    const time::RationalTime& delta) const;

    /// Shift every clip starting at or after `from` by `delta`. This is ripple.
    void shiftFrom(const time::RationalTime& from, const time::RationalTime& delta);

    /// Direct access for commands that restore a whole track from a snapshot.
    void setClips(std::vector<Clip> clips);

    friend bool operator==(const Track&, const Track&) = default;

private:
    void checkInvariants() const;

    TrackId id_;
    TrackKind kind_{TrackKind::Video};
    std::string name_;
    std::vector<Clip> clips_;
    std::vector<Transition> transitions_;
    bool muted_{false};
    bool locked_{false};
    /// On by default: tracks following a ripple is the behaviour that keeps a
    /// cut in sync, and it should have to be turned off deliberately.
    bool syncLocked_{true};
    double gainDb_{0.0};
    double pan_{0.0};
};

}  // namespace zaro::model
