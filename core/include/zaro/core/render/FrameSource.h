#pragma once

#include "zaro/core/Error.h"
#include "zaro/core/media/AudioBuffer.h"
#include "zaro/core/model/Ids.h"
#include "zaro/core/render/RgbaImage.h"
#include "zaro/core/time/RationalTime.h"

namespace zaro::render {

/// Where the compositor gets pixels from.
///
/// This is the seam that keeps the render graph in core and out of FFmpeg. The
/// real implementation decodes and caches; tests use a synthetic one that
/// generates known patterns, which is what makes the compositor verifiable in
/// CI on a machine with no media and no GPU.
class FrameSource {
public:
    virtual ~FrameSource() = default;

    FrameSource(const FrameSource&) = delete;
    FrameSource& operator=(const FrameSource&) = delete;

    /// The frame for `media` at `sourceTime`, already converted to the linear
    /// working space.
    ///
    /// The returned image stays valid until the next call on this source. The
    /// compositor draws each clip before asking for the next, so it never needs
    /// two at once, and the narrow contract lets an implementation hand back a
    /// pointer into its cache rather than a copy.
    [[nodiscard]] virtual Result<const RgbaImage*> imageFor(
        model::MediaRefId media, const time::RationalTime& sourceTime) = 0;

protected:
    FrameSource() = default;
};

/// Where the mixer gets samples from.
class AudioSource {
public:
    virtual ~AudioSource() = default;

    AudioSource(const AudioSource&) = delete;
    AudioSource& operator=(const AudioSource&) = delete;

    /// Fill `out` with `sampleCount` samples starting at `sourceStart`,
    /// resampled to `sampleRate`. Reading past the end of the media fills the
    /// remainder with silence rather than failing: a clip trimmed to the last
    /// frame should not make the whole render fall over.
    [[nodiscard]] virtual Status read(model::MediaRefId media,
                                      const time::RationalTime& sourceStart,
                                      std::int64_t sampleCount, const time::Rational& sampleRate,
                                      media::AudioBuffer& out) = 0;

protected:
    AudioSource() = default;
};

}  // namespace zaro::render
