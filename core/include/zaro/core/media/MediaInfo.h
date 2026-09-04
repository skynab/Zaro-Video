#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "zaro/core/media/ColorInfo.h"
#include "zaro/core/media/PixelFormat.h"
#include "zaro/core/time/RationalTime.h"
#include "zaro/core/time/TimeRange.h"
#include "zaro/core/time/Timecode.h"

namespace zaro::media {

/// How long a still is when nobody has said otherwise.
///
/// A photograph has no duration of its own, so one has to be chosen the moment
/// it lands on a timeline. Five seconds is the length every editor picks for
/// this and it is long enough to see and short enough to trim, which is all a
/// default has to be -- the clip is stretchable from the instant it exists, so
/// getting this wrong costs one drag.
inline constexpr std::int64_t kDefaultStillSeconds = 5;

struct VideoStreamInfo {
    std::int32_t streamIndex{-1};
    std::string codecName;

    std::int32_t width{0};
    std::int32_t height{0};
    PixelFormat pixelFormat{PixelFormat::Unknown};
    ColorInfo color{};
    /// True when `color` was filled in by convention rather than read from the
    /// file. Worth surfacing in the UI, because the guess is sometimes wrong.
    bool colorWasGuessed{false};

    /// The nominal rate, from container metadata.
    time::Rational frameRate{time::rates::fps24};
    /// The average rate actually observed. Diverging from `frameRate` is the
    /// first sign of variable frame rate footage.
    time::Rational averageFrameRate{time::rates::fps24};
    bool isVariableFrameRate{false};

    time::Rational pixelAspect{1, 1};
    /// Display rotation from container metadata, in degrees clockwise. Phone
    /// footage is routinely stored sideways with this as the only hint.
    std::int32_t rotationDegrees{0};

    time::Rational duration{0, 1};
    /// Container-reported frame count. Often absent, sometimes wrong; treat as
    /// a hint rather than as truth.
    std::int64_t frameCountHint{0};

    /// One picture that lasts as long as somebody wants it to.
    ///
    /// A .png or a .jpg is a video stream of exactly one frame, and FFmpeg
    /// reports it as one: a container called `png_pipe` or `image2`, a rate
    /// invented out of nothing, and either no duration or one frame's worth.
    /// Taken literally that makes a clip a fortieth of a second long, which is
    /// not what anybody dragging a photograph onto a timeline means.
    ///
    /// So a still is marked here and treated as *unbounded* source: it can be
    /// stretched to any length, because there is no more of it to run out of.
    /// Everything else about it stays a video clip -- it is placed, trimmed,
    /// moved, graded, masked, transformed and keyframed by exactly the code
    /// that does those things to footage, which is the whole point of spelling
    /// it as a flag on the stream rather than as a kind of its own.
    bool isStill{false};

    std::optional<time::Timecode> startTimecode;

    [[nodiscard]] time::RationalTime durationInFrames() const {
        return time::RationalTime::fromSeconds(duration, frameRate);
    }
};

struct AudioStreamInfo {
    std::int32_t streamIndex{-1};
    std::string codecName;

    time::Rational sampleRate{48000, 1};
    std::int32_t channelCount{0};
    std::string channelLayout;
    time::Rational duration{0, 1};
    std::int64_t sampleCountHint{0};
};

struct MediaInfo {
    std::string path;
    std::string formatName;
    time::Rational duration{0, 1};
    std::int64_t bitRate{0};

    std::vector<VideoStreamInfo> videoStreams;
    std::vector<AudioStreamInfo> audioStreams;

    [[nodiscard]] const VideoStreamInfo* primaryVideo() const {
        return videoStreams.empty() ? nullptr : &videoStreams.front();
    }
    [[nodiscard]] const AudioStreamInfo* primaryAudio() const {
        return audioStreams.empty() ? nullptr : &audioStreams.front();
    }

    /// Whether this file is a single picture rather than footage.
    ///
    /// Asked of the file rather than of the stream because that is how callers
    /// think of it: a .jpg is a still, and the fact that the stillness lives on
    /// its one video stream is a detail of how it was probed.
    [[nodiscard]] bool isStill() const {
        const VideoStreamInfo* video = primaryVideo();
        return video != nullptr && video->isStill && audioStreams.empty();
    }
};

}  // namespace zaro::media
