#pragma once

#include "zaro/core/media/AudioBuffer.h"
#include "zaro/core/model/Sequence.h"
#include "zaro/core/render/FrameSource.h"

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

private:
    AudioSource* source_;
    std::int32_t lastClipCount_{0};
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
