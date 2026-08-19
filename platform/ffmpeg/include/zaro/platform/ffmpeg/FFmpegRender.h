#pragma once

#include <functional>
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

/// Everything a render needs beyond the project itself.
struct RenderRequest {
    std::string outputPath;
    model::SequenceId sequence;
    /// First frame, and how many. A count of -1 means to the end.
    std::int64_t startFrame{0};
    std::int64_t frameCount{-1};
    bool includeAudio{true};
    std::size_t cacheBudgetBytes{64u * 1024u * 1024u};
    /// Composite on the GPU where it is available. Falls back on its own.
    bool preferGpu{true};
};

/// What the encoder actually wrote.
///
/// Kept because the gap between frames encoded and packets written is what
/// caught an export that was silently one frame short: the container reported
/// the right frame count while the last packet had a duration of zero.
struct RenderSummary {
    std::int64_t framesEncoded{0};
    std::int64_t videoPacketsWritten{0};
    std::int64_t audioSamplesWritten{0};
    std::int64_t audioSamplesExpected{0};
    std::uint64_t cacheHits{0};
    std::uint64_t cacheMisses{0};
};

struct RenderProgress {
    std::int64_t framesDone{0};
    std::int64_t framesTotal{0};
    double elapsedSeconds{0.0};
};

/// Render a sequence to a file.
///
/// Extracted so the command-line renderer and the export dialog run the same
/// code rather than two loops that have to be kept agreeing. `onProgress` is
/// called from the calling thread; `keepGoing` is polled per frame and
/// abandoning leaves a partial file, which the caller should remove.
[[nodiscard]] Status renderSequence(
    const model::Project& project, const RenderRequest& request,
    const std::function<void(const RenderProgress&)>& onProgress = {},
    const std::function<bool()>& keepGoing = {}, RenderSummary* summary = nullptr);

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
