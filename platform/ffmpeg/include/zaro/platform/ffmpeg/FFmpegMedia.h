#pragma once

#include <memory>
#include <optional>
#include <string>

#include "zaro/core/Error.h"
#include "zaro/core/media/Decoder.h"
#include "zaro/core/media/MediaInfo.h"
#include "zaro/core/media/VideoFrame.h"

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

/// Route libav's own logging into ours, and quieten it. Call once at startup.
void installLogHandler(bool verbose = false);

}  // namespace zaro::platform::ffmpeg
