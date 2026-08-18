#pragma once

#include <memory>
#include <string>

#include "zaro/core/Error.h"
#include "zaro/core/model/Project.h"
#include "zaro/core/render/FrameCache.h"
#include "zaro/core/render/FrameSource.h"

namespace zaro::platform::ffmpeg {

/// Feeds the render graph from a project's media, decoding on demand.
///
/// Decoders are opened lazily and kept open, because a render walks each clip's
/// source in order and reopening per frame would dominate the cost. Frames are
/// cached in the working space rather than as decoded Y'CbCr, so a frame
/// revisited during scrubbing skips both the decode and the colour conversion.
class ProjectMediaSource final : public render::FrameSource,
                                 public render::SourceFrameProvider,
                                 public render::AudioSource {
public:
    static Result<std::unique_ptr<ProjectMediaSource>> open(
        const model::Project& project,
        std::size_t cacheBudgetBytes = render::FrameCache::kDefaultBudgetBytes);

    ~ProjectMediaSource() override;

    [[nodiscard]] Result<const render::RgbaImage*> imageFor(
        model::MediaRefId media, const time::RationalTime& sourceTime) override;

    [[nodiscard]] Result<const media::VideoFrame*> sourceFrameFor(
        model::MediaRefId media, const time::RationalTime& sourceTime) override;

    [[nodiscard]] Status read(model::MediaRefId media, const time::RationalTime& sourceStart,
                              std::int64_t sampleCount, const time::Rational& sampleRate,
                              media::AudioBuffer& out) override;

    [[nodiscard]] const render::FrameCache& cache() const;

private:
    ProjectMediaSource();

    struct State;
    std::unique_ptr<State> state_;
};

struct EncodeSettings {
    std::string path;
    std::int32_t width{1920};
    std::int32_t height{1080};
    time::Rational frameRate{time::rates::fps25};

    time::Rational audioSampleRate{time::rates::hz48000};
    std::int32_t audioChannels{2};
    bool includeAudio{true};

    /// Empty picks from the container: ProRes and PCM for .mov, H.264 and AAC
    /// for .mp4. PCM is the default for .mov specifically because it has no
    /// encoder delay, which is what makes sample-exact output verifiable.
    std::string videoCodec;
    std::string audioCodec;
    std::int64_t videoBitRate{0};
};

/// Writes a rendered sequence to a file.
///
/// Timestamps are derived from counters, not accumulated from durations: the
/// video PTS is the frame index and the audio PTS is the running sample count,
/// each on its own exact timebase. Nothing here can drift, because nothing here
/// adds a duration to a running total.
class Encoder {
public:
    static Result<std::unique_ptr<Encoder>> open(const EncodeSettings& settings);
    ~Encoder();

    [[nodiscard]] Status writeVideo(const render::RgbaImage& frame);
    [[nodiscard]] Status writeAudio(const media::AudioBuffer& samples);
    /// Flushes both encoders and writes the trailer. Must be called; the
    /// destructor cannot report a failure.
    [[nodiscard]] Status finish();

    [[nodiscard]] std::int64_t framesWritten() const;
    /// Packets actually handed to the muxer. Should equal framesWritten() once
    /// finish() has flushed; a gap means frames were encoded but not written.
    [[nodiscard]] std::int64_t videoPacketsWritten() const;
    [[nodiscard]] std::int64_t samplesWritten() const;

private:
    Encoder();

    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace zaro::platform::ffmpeg
