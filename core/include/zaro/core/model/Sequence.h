#pragma once

#include <string>
#include <vector>

#include "zaro/core/model/Track.h"
#include "zaro/core/time/Rational.h"

namespace zaro::model {

/// A cut: tracks, plus the format everything on them is rendered to.
///
/// Video tracks stack bottom-up -- index 0 is V1, the lowest, and later indices
/// composite over it. Audio tracks are independent lanes that sum.
class Sequence {
public:
    Sequence() = default;
    Sequence(SequenceId id, std::string name, time::Rational frameRate)
        : id_{id}, name_{std::move(name)}, frameRate_{std::move(frameRate)} {}

    [[nodiscard]] SequenceId id() const noexcept { return id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void setName(std::string value) { name_ = std::move(value); }

    [[nodiscard]] const time::Rational& frameRate() const noexcept { return frameRate_; }
    [[nodiscard]] const time::Rational& audioSampleRate() const noexcept {
        return audioSampleRate_;
    }
    void setAudioSampleRate(time::Rational value) { audioSampleRate_ = std::move(value); }

    [[nodiscard]] std::int32_t width() const noexcept { return width_; }
    [[nodiscard]] std::int32_t height() const noexcept { return height_; }
    void setSize(std::int32_t width, std::int32_t height) {
        width_ = width;
        height_ = height;
    }

    /// Where timecode starts. Broadcast deliverables routinely begin at
    /// 01:00:00:00 rather than zero, and the offset has to survive round trips.
    [[nodiscard]] const time::RationalTime& startTime() const noexcept { return startTime_; }
    void setStartTime(time::RationalTime value) { startTime_ = std::move(value); }

    [[nodiscard]] const std::vector<Track>& videoTracks() const noexcept { return videoTracks_; }
    [[nodiscard]] const std::vector<Track>& audioTracks() const noexcept { return audioTracks_; }

    [[nodiscard]] Track* findTrack(TrackId id);
    [[nodiscard]] const Track* findTrack(TrackId id) const;

    TrackId addTrack(TrackId id, TrackKind kind, std::string name);
    void removeTrack(TrackId id);

    /// Mutable access for edit commands. Deliberately awkward to reach for from
    /// ordinary code: all editing should go through edit/Operations.
    [[nodiscard]] std::vector<Track>& tracksMutable(TrackKind kind) {
        return kind == TrackKind::Video ? videoTracks_ : audioTracks_;
    }

    /// End of the last clip on any track -- the length of the cut.
    [[nodiscard]] time::RationalTime duration() const;

    friend bool operator==(const Sequence&, const Sequence&) = default;

private:
    SequenceId id_;
    std::string name_;
    time::Rational frameRate_{time::rates::fps24};
    time::Rational audioSampleRate_{time::rates::hz48000};
    std::int32_t width_{1920};
    std::int32_t height_{1080};
    time::RationalTime startTime_{};
    std::vector<Track> videoTracks_;
    std::vector<Track> audioTracks_;
};

}  // namespace zaro::model
