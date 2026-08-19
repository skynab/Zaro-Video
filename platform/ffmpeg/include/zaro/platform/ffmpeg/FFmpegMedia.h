#pragma once

#include <memory>
#include <optional>
#include <string>

#include "zaro/core/Error.h"
#include "zaro/core/media/Decoder.h"
#include "zaro/core/media/MediaInfo.h"
#include "zaro/core/media/VideoFrame.h"
#include "zaro/core/media/Waveform.h"

/// The FFmpeg backend. This is the only part of the codebase that includes
/// libav* headers; everything above it sees the interfaces in core/media.
namespace zaro::platform::ffmpeg {

/// Read a file's structure without decoding any of it.
[[nodiscard]] Result<media::MediaInfo> probe(const std::string& path);

[[nodiscard]] Result<std::unique_ptr<media::VideoDecoder>> openVideoDecoder(
    const std::string& path, const media::DecoderOptions& options = {});

/// `outputSampleRate` resamples on the way out; unset delivers the file's own
/// rate. Channel order and float format are always canonical.
[[nodiscard]] Result<std::unique_ptr<media::AudioDecoder>> openAudioDecoder(
    const std::string& path, const media::DecoderOptions& options = {},
    std::optional<time::Rational> outputSampleRate = std::nullopt);

/// Write a frame to a PNG, converting to RGB through the frame's own colour
/// tags so limited-range and BT.601 material does not come out shifted.
[[nodiscard]] Status writePng(const media::VideoFrame& frame, const std::string& path);

/// Write a frame's planes verbatim, rows packed with no stride padding. This is
/// the form `ffmpeg -f rawvideo` produces, which makes byte-for-byte comparison
/// against FFmpeg's own decoder possible.
[[nodiscard]] Status writeRawPlanes(const media::VideoFrame& frame, const std::string& path);

/// Decode a file's audio and reduce it to peaks.
///
/// Reads the whole stream, so it is slow enough to belong on a background
/// thread and is why the result is cached.
[[nodiscard]] Result<media::Waveform> buildWaveform(const std::string& path,
                                                    std::int64_t samplesPerBucket = 512);

/// Peaks on disk, keyed by a file's quick content hash.
///
/// The cache is what makes waveforms usable: generating them takes about as
/// long as decoding the audio, which is unacceptable every time a project is
/// opened and fine once. A stale entry is not a correctness problem -- the key
/// includes size and modification time, so an edited file simply misses and is
/// rebuilt.
class WaveformStore {
public:
    explicit WaveformStore(std::string directory);

    /// From memory, then from disk, then by decoding. Whatever it takes.
    [[nodiscard]] Result<media::Waveform> get(const std::string& path,
                                              std::int64_t samplesPerBucket = 512);

    [[nodiscard]] const std::string& directory() const noexcept { return directory_; }

private:
    std::string directory_;
};

/// Route libav's own logging into ours, and quieten it. Call once at startup.
void installLogHandler(bool verbose = false);

}  // namespace zaro::platform::ffmpeg
