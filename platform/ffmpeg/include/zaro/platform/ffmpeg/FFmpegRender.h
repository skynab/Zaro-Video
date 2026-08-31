#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "zaro/core/Error.h"
#include "zaro/core/model/Project.h"
#include "zaro/core/render/FrameCache.h"
#include "zaro/core/render/FrameSource.h"
#include "zaro/core/render/TextRasterizer.h"

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

    /// Correct what the container claimed about a frame's curve, before
    /// anything reads it.
    void applyOverride(model::MediaRefId media, media::VideoFrame& frame) const;

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
    /// Copy the source's packets when the export turns out to be a piece of a
    /// file with nothing done to it. Off is how somebody forces a re-encode --
    /// for a file another program will not accept, say.
    bool allowCopy{true};

    /// What to encode with. Empty picks from the container, which is what
    /// every caller did before there was a choice: ProRes and PCM for .mov,
    /// H.264 and AAC for .mp4.
    ///
    /// Named rather than enumerated because these are FFmpeg encoder names and
    /// which ones exist depends on how FFmpeg was built. A name this build does
    /// not have fails when the encoder is opened, with the name in the message
    /// -- which is more use than a menu that offers something and then cannot
    /// say what went wrong.
    std::string videoCodec;
    std::string audioCodec;
    /// Bits per second for the picture. Zero leaves it to the encoder, and
    /// codecs that do not take a rate -- ProRes picks its own from the profile
    /// -- ignore it.
    std::int64_t videoBitRate{0};
};

/// What the encoder actually wrote.
///
/// Kept because the gap between frames encoded and packets written is what
/// caught an export that was silently one frame short: the container reported
/// the right frame count while the last packet had a duration of zero.
struct RenderSummary {
    std::int64_t framesEncoded{0};
    /// Text layers that could not be drawn because there was no font engine.
    std::int64_t textLayersSkipped{0};
    std::int64_t videoPacketsWritten{0};
    std::int64_t audioSamplesWritten{0};
    std::int64_t audioSamplesExpected{0};
    std::uint64_t cacheHits{0};
    std::uint64_t cacheMisses{0};

    /// Whether the export was a copy rather than a re-encode.
    bool copied{false};
    /// Why it was or was not, in words. Always filled in: an export that
    /// quietly re-encoded would leave somebody wondering why it took twenty
    /// minutes, and one that quietly copied would leave them wondering why the
    /// grade is missing.
    std::string copyReason;
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
/// `text` is how to draw text layers. Null renders everything else and reports
/// the count it skipped in the summary — a delivered file quietly missing its
/// titles is the worst outcome available, so the number is on the record.
[[nodiscard]] Status renderSequence(
    const model::Project& project, const RenderRequest& request,
    const std::function<void(const RenderProgress&)>& onProgress = {},
    const std::function<bool()>& keepGoing = {}, RenderSummary* summary = nullptr,
    render::TextRasterizer* text = nullptr);

/// What a copy wrote.
struct CopySummary {
    std::int64_t videoPackets{0};
    std::int64_t audioPackets{0};
};

/// Copy a range of a file into a new one without decoding it.
///
/// The packets are written as they are, with their timestamps rebased so the
/// output starts at zero. Nothing is re-encoded, so the picture is the file's
/// own picture and the export costs what a file copy costs.
///
/// Refuses when the range does not start on a keyframe: beginning anywhere else
/// would hand a decoder frames that refer back to pictures the new file does
/// not contain. The caller falls back to an ordinary render, which is always
/// able to produce the frames.
///
/// Whether a copy is *allowed* -- whether the export is really just a piece of
/// this file -- is `render::smartRenderPlan`, in core, where it can be tested
/// without a media file.
/// `onFrames` is called as picture packets go out and `keepGoing` is polled per
/// packet: a copy of an hour-long file is fast but not instant, and neither a
/// progress bar that jumps from nothing to done nor a cancel that takes effect
/// at the end is worth having.
[[nodiscard]] Result<CopySummary> copyRange(const std::string& sourcePath,
                                            const std::string& outputPath,
                                            const time::RationalTime& sourceStart,
                                            std::int64_t frames, bool includeAudio,
                                            const std::function<void(std::int64_t)>& onFrames = {},
                                            const std::function<bool()>& keepGoing = {});

/// What a proxy should be, and where it goes.
struct ProxySettings {
    std::string source;
    std::string destination;

    /// The width to aim for; the height follows the source's shape. Both are
    /// rounded to even numbers, because every codec worth making a proxy in
    /// subsamples chroma and cannot represent an odd one.
    ///
    /// Zero keeps the source's own size, which is what turns this from making
    /// a proxy into transcoding on ingest: the same operation, told to change
    /// the codec and not the size.
    std::int32_t width{960};

    /// Empty means H.264, not the container's default: a .mov defaults to
    /// ProRes, which is right for a deliverable and absurd for a proxy.
    std::string videoCodec;
    std::int64_t videoBitRate{0};
};

struct ProxySummary {
    std::string path;
    std::int32_t width{0};
    std::int32_t height{0};
    std::int64_t frames{0};
    std::uint64_t sourceBytes{0};
    std::uint64_t proxyBytes{0};
};

/// Make another copy of a media file, frame for frame -- smaller, in a
/// different codec, or both.
///
/// A proxy and an ingest transcode are this one operation with different
/// settings: a proxy is smaller in an easy codec, an ingest transcode is the
/// same size in an easy codec. Writing them as two functions would be two
/// places for the frame-count rule below to be got wrong.
///
/// **The same rate and the same number of frames, always.** A proxy is a
/// stand-in, and the one thing a stand-in must not do is change where the cuts
/// land: `MediaRef::proxyPath` says two files have to describe the same thing,
/// and a proxy a frame shorter would silently retime every clip that used it.
/// So the frame count is taken from the source and the loop writes exactly
/// that many, whatever the decoder does at the end of the file.
///
/// **Audio comes too.** Proxies are switched on for the whole project at once,
/// and a video-only proxy would mean turning them on silences the timeline.
///
/// **Through the same decode and encode path as an export.** A proxy made by a
/// second, simpler pipeline would be a second place for colour handling to be
/// wrong, and the case where it was wrong would be the one where somebody was
/// editing against the proxy and grading against the original.
[[nodiscard]] Result<ProxySummary> makeProxy(const ProxySettings& settings,
                                             const std::function<void(double)>& onProgress = {},
                                             const std::function<bool()>& keepGoing = {});

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

    /// Keep the coverage, for delivering a graphic over nothing.
    ///
    /// Only ProRes 4444 carries it here, which is what a title or a logo is
    /// handed over as. Asked for on a codec that cannot hold an alpha channel
    /// this is refused rather than ignored: an export that silently composites
    /// a lower third onto black looks like it worked, and is discovered by
    /// whoever tries to use it.
    bool alpha{false};

    /// What the deliverable is encoded through, and where its highlights start
    /// rolling off. Taken from the sequence's `output()`, so what is exported
    /// is what the scopes and the curve editor were drawn against.
    ///
    /// A knee of 1 means no rolloff and the encoder clips, which is what this
    /// program did before there was a choice -- and is why an existing project
    /// exports the same file it always did.
    media::TransferFunction transfer{media::TransferFunction::BT709};
    double highlightKnee{1.0};
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
    /// Flushes both encoders, writes the trailer and closes the file. Must be
    /// called; the destructor cannot report a failure.
    ///
    /// The file is closed here and not left to the destructor, so that the
    /// output can be renamed or removed the moment this returns -- on Windows
    /// an open handle forbids both.
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
