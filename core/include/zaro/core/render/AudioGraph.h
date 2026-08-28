#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

#include "zaro/core/media/AudioBuffer.h"
#include "zaro/core/model/Project.h"
#include "zaro/core/model/Sequence.h"
#include "zaro/core/render/AudioProcessor.h"
#include "zaro/core/render/FrameSource.h"
#include "zaro/core/render/Loudness.h"

namespace zaro::render {

/// Sums a sequence's audio tracks over a span of timeline.
///
/// Works in whole samples at the sequence's audio rate throughout. A block
/// boundary that lands half a sample out is inaudible once; repeated fifty
/// times a second for an hour it is a drift, and drift against picture is the
/// failure mode that makes an editor unusable.
class AudioGraph {
public:
    explicit AudioGraph(AudioSource& source) : source_{&source} {}

    /// Mix `sampleCount` samples starting at `start`, which is a timeline time.
    [[nodiscard]] Result<media::AudioBuffer> mix(const model::Sequence& sequence,
                                                 const time::RationalTime& start,
                                                 std::int64_t sampleCount,
                                                 std::int32_t channelCount = 2);

    [[nodiscard]] std::int32_t lastClipCount() const noexcept { return lastClipCount_; }

    /// The project a nested clip's sequence is looked up in. Without it a
    /// nested clip contributes silence, the same way it draws nothing.
    void setProject(const model::Project* project) { project_ = project; }

    /// The loudest sample in the last mix, per track and overall.
    ///
    /// Peak rather than RMS: a mixer's meter exists to answer "is this about to
    /// clip", and RMS answers a different question — it can sit comfortably
    /// while individual samples are over. A loudness meter is a separate
    /// instrument and belongs with the loudness work, not here.
    ///
    /// Measured on the way through rather than by scanning the result: the
    /// samples are already in registers, and a second pass over every block
    /// would cost more than the mixing does.
    struct Meters {
        /// Per track, keyed by id. Post-fader: what the track contributes, not
        /// what its clips hold.
        std::map<std::uint64_t, float> tracks;
        /// The summed output, per channel.
        std::vector<float> master;
        /// How much the compressor pulled each track down, in decibels. Never
        /// positive, and zero where nothing is compressing.
        std::map<std::uint64_t, float> reduction;

        [[nodiscard]] float peakFor(model::TrackId track) const {
            const auto found = tracks.find(track.value());
            return found == tracks.end() ? 0.0F : found->second;
        }
        [[nodiscard]] float masterPeak() const {
            return master.empty() ? 0.0F : *std::max_element(master.begin(), master.end());
        }
    };

    [[nodiscard]] const Meters& meters() const noexcept { return meters_; }

    /// Forget every filter's delay line and every compressor's envelope.
    ///
    /// **Processing has state, so mixing is no longer a pure function of
    /// time.** That is the point of a compressor — but it means a seek has to
    /// reset the chain, or the envelope from one part of the timeline follows
    /// the playhead to another and the first second after a jump is ducked for
    /// no reason. Anything that moves the playhead calls this.
    void resetProcessing();

    /// What a sequence measures, over a range.
    struct LoudnessResult {
        double integratedLufs{LoudnessMeter::kSilence};
        double samplePeakDbfs{-180.0};
        /// What to add to hit a target, in decibels. Zero for silence, which
        /// has no gain that would make it loud.
        [[nodiscard]] double gainToReach(double targetLufs) const {
            return integratedLufs <= LoudnessMeter::kSilence ? 0.0 : targetLufs - integratedLufs;
        }
    };

    /// Measure a sequence by mixing it.
    ///
    /// Through the real mix, so what is measured is what will be delivered:
    /// faders, pans, automation, the processing chain and every clip gain are
    /// all in it. Measuring the clips instead would give a number about the
    /// material rather than about the programme.
    [[nodiscard]] Result<LoudnessResult> measureLoudness(const model::Sequence& sequence,
                                                         const time::TimeRange& range,
                                                         std::int32_t channelCount = 2);

private:
    AudioSource* source_;
    const model::Project* project_{nullptr};
    std::int32_t depth_{0};
    std::int32_t lastClipCount_{0};
    Meters meters_;
    /// One processing chain per track, kept between blocks because a filter's
    /// delay line and a compressor's envelope are exactly the state that makes
    /// them work.
    std::map<std::uint64_t, TrackProcessor> processors_;
};

/// Decibels to a linear factor. -inf and anything below the floor become
/// silence rather than a denormal.
[[nodiscard]] float gainFromDb(double decibels) noexcept;

/// Constant-power pan, for placing a **mono** source in the stereo field.
/// A centred signal keeps the same apparent loudness as it moves, which a
/// linear pan does not -- linear dips by 3 dB in the middle. The cost is that
/// centre is 0.707 per side rather than unity.
void panGains(double pan, float& leftGain, float& rightGain) noexcept;

/// Balance, for shifting an **already stereo** signal. Unity at centre, and it
/// attenuates one side as it moves rather than recentring the image.
///
/// The distinction matters: applying the constant-power pan law a second time
/// to a signal that has already been placed pulls another 3 dB out of it, so a
/// clip and its track both sitting at centre would come out half as loud as
/// they went in.
void balanceGains(double pan, float& leftGain, float& rightGain) noexcept;

}  // namespace zaro::render
