#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "zaro/core/Error.h"
#include "zaro/core/media/AudioBuffer.h"
#include "zaro/core/media/MediaInfo.h"
#include "zaro/core/media/VideoFrame.h"

namespace zaro::media {

enum class DecodeMode {
    /// Whichever path is actually fastest for the current pipeline.
    ///
    /// Today that is software, which is not the obvious answer. Hardware decode
    /// produces frames in GPU memory, and every one of them then has to be
    /// copied back to system memory for a CPU-side consumer. That readback
    /// costs more than the decode it saves: measured on Apple silicon, 4K
    /// ProRes runs at 5.8x realtime in software and 1.0x through VideoToolbox,
    /// and 1080p H.264 at 49x versus 5.7x.
    ///
    /// This flips in Phase 3, when the compositor can take a GPU texture
    /// directly and the readback disappears. See docs/adr/0003.
    Auto,
    ForceSoftware,  ///< Reference path, and the one tests compare against.
    ForceHardware,  ///< Fail rather than silently fall back.
};

struct DecoderOptions {
    DecodeMode mode{DecodeMode::Auto};
    /// 0 lets the decoder choose. Relevant only to software decoding.
    std::int32_t threadCount{0};
    /// Which stream to open; -1 picks the container's best.
    std::int32_t streamIndex{-1};
};

/// Random-access video decode.
///
/// The interface lives in core and the implementation does not, so the edit
/// engine can be built and tested without FFmpeg, and so a second backend
/// (AVFoundation, or a GPU-resident decoder in Phase 3) can be dropped in
/// without touching a caller.
class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    [[nodiscard]] virtual const VideoStreamInfo& info() const = 0;

    /// Whether frames are actually coming back from the hardware decoder. Not
    /// the same as having asked for it: hardware decode declines silently for
    /// unsupported profiles, and knowing which path ran matters when a
    /// performance measurement looks wrong.
    [[nodiscard]] virtual bool usingHardware() const = 0;

    /// Decode the next frame in presentation order. Returns
    /// `ErrorCode::EndOfStream` once exhausted -- an ordinary outcome, not a
    /// failure.
    [[nodiscard]] virtual Result<VideoFrame> nextFrame() = 0;

    /// The frame whose presentation interval contains `t`: the last frame whose
    /// timestamp is at or before it. This is what a playhead at `t` should show,
    /// and it is well defined for variable frame rate footage where a frame
    /// index alone is not.
    [[nodiscard]] virtual Result<VideoFrame> frameAtTime(const time::RationalTime& t) = 0;

    /// The frame at source index `index`, counted in presentation order from
    /// zero. Exact for variable frame rate footage, which requires knowing every
    /// frame's timestamp -- see `conform()`.
    [[nodiscard]] virtual Result<VideoFrame> frameAtIndex(std::int64_t index) = 0;

    /// Scan the container to learn every frame's exact timestamp.
    ///
    /// Containers lie about frame counts and variable frame rate footage has no
    /// arithmetic relationship between index and time, so the only way to be
    /// frame-exact is to look. This reads packets without decoding them, which
    /// is fast, but it does touch the whole file -- hence an explicit call
    /// rather than a hidden cost inside a seek. This is what other editors call
    /// conforming.
    [[nodiscard]] virtual Status conform() = 0;
    [[nodiscard]] virtual bool isConformed() const = 0;

    /// Exact frame count. Conforms first if it has not already happened.
    [[nodiscard]] virtual Result<std::int64_t> frameCount() = 0;

protected:
    VideoDecoder() = default;
};

/// Sequential audio decode into the canonical planar float format.
class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    [[nodiscard]] virtual const AudioStreamInfo& info() const = 0;

    /// The rate samples are delivered at, which may differ from the file's if
    /// resampling was requested.
    [[nodiscard]] virtual const time::Rational& outputSampleRate() const = 0;

    [[nodiscard]] virtual Result<AudioBuffer> nextBuffer() = 0;
    [[nodiscard]] virtual Status seek(const time::RationalTime& t) = 0;

protected:
    AudioDecoder() = default;
};

}  // namespace zaro::media
