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
};

}  // namespace zaro::media
