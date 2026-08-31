#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "zaro/core/render/FrameSource.h"

namespace zaro::testing {

/// A FrameSource that generates flat colours instead of decoding anything.
///
/// The compositor can then be verified exactly, in CI, on a machine with no
/// media and no GPU: if a clip is registered as pure red, a pixel that is not
/// pure red is a compositing bug and nothing else.
class SolidFrameSource final : public render::FrameSource {
public:
    SolidFrameSource(std::int32_t width, std::int32_t height) : width_{width}, height_{height} {}

    void define(model::MediaRefId media, const render::Rgba& colour) {
        colours_[media.value()] = colour;
    }

    /// A source whose brightness says which frame it is, so a test can tell
    /// which frame was actually fetched rather than only that one was.
    void defineRamp(model::MediaRefId media) { ramps_.insert(media.value()); }

    /// Requests are recorded so tests can assert *which* source time was asked
    /// for -- the mapping from timeline to source is as easy to get wrong as
    /// the pixels, and much harder to see.
    [[nodiscard]] const std::vector<time::RationalTime>& requests() const { return requests_; }

    Result<const render::RgbaImage*> imageFor(model::MediaRefId media,
                                              const time::RationalTime& sourceTime) override {
        requests_.push_back(sourceTime);
        current_ = render::RgbaImage{width_, height_};
        if (ramps_.count(media.value()) != 0) {
            const auto level = static_cast<float>(sourceTime.frames()) / 1000.0F;
            current_.fill(render::Rgba{level, level, level, 1.0F});
            return &current_;
        }
        const auto found = colours_.find(media.value());
        if (found == colours_.end()) {
            return Error{ErrorCode::NotFound, "no such media in this test source"};
        }
        current_.fill(found->second);
        return &current_;
    }

private:
    std::int32_t width_;
    std::int32_t height_;
    std::map<std::uint64_t, render::Rgba> colours_;
    std::set<std::uint64_t> ramps_;
    render::RgbaImage current_;
    std::vector<time::RationalTime> requests_;
};

/// An AudioSource generating a constant value per media reference, so gain and
/// pan arithmetic can be checked against exact expected numbers.
class ConstantAudioSource final : public render::AudioSource {
public:
    void define(model::MediaRefId media, float value, std::int32_t channels = 2) {
        values_[media.value()] = {value, channels, {}};
    }

    /// A different constant in each channel, so a test can tell which of them
    /// arrived. The channel count is the number of values given.
    void defineChannels(model::MediaRefId media, std::vector<float> perChannel) {
        const auto channels = static_cast<std::int32_t>(perChannel.size());
        values_[media.value()] = {0.0F, channels, std::move(perChannel)};
    }

    /// Truncate every read to this many samples, to exercise the short-read
    /// path where a clip runs past the end of its media.
    void setAvailableSamples(std::int64_t samples) { available_ = samples; }

    Status read(model::MediaRefId media, const time::RationalTime& sourceStart,
                std::int64_t sampleCount, const time::Rational& sampleRate,
                media::AudioBuffer& out) override {
        lastSourceStart = sourceStart;
        const auto found = values_.find(media.value());
        if (found == values_.end()) {
            return Error{ErrorCode::NotFound, "no such media in this test source"};
        }
        const std::int64_t produced =
            available_ >= 0 ? std::min(available_, sampleCount) : sampleCount;
        out = media::AudioBuffer{found->second.channels, produced, sampleRate};
        for (std::int32_t channel = 0; channel < found->second.channels; ++channel) {
            float* samples = out.channel(channel);
            const std::vector<float>& each = found->second.perChannel;
            const float value =
                each.empty() ? found->second.value : each[static_cast<std::size_t>(channel)];
            for (std::int64_t i = 0; i < produced; ++i) {
                samples[i] = value;
            }
        }
        return {};
    }

    time::RationalTime lastSourceStart;

private:
    struct Entry {
        float value;
        std::int32_t channels;
        std::vector<float> perChannel;
    };
    std::map<std::uint64_t, Entry> values_;
    std::int64_t available_{-1};
};

}  // namespace zaro::testing
