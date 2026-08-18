#pragma once

#include <cmath>
#include <map>
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

    /// Requests are recorded so tests can assert *which* source time was asked
    /// for -- the mapping from timeline to source is as easy to get wrong as
    /// the pixels, and much harder to see.
    [[nodiscard]] const std::vector<time::RationalTime>& requests() const { return requests_; }

    Result<const render::RgbaImage*> imageFor(model::MediaRefId media,
                                              const time::RationalTime& sourceTime) override {
        requests_.push_back(sourceTime);
        const auto found = colours_.find(media.value());
        if (found == colours_.end()) {
            return Error{ErrorCode::NotFound, "no such media in this test source"};
        }
        current_ = render::RgbaImage{width_, height_};
        current_.fill(found->second);
        return &current_;
    }

private:
    std::int32_t width_;
    std::int32_t height_;
    std::map<std::uint64_t, render::Rgba> colours_;
    render::RgbaImage current_;
    std::vector<time::RationalTime> requests_;
};

/// An AudioSource generating a constant value per media reference, so gain and
/// pan arithmetic can be checked against exact expected numbers.
class ConstantAudioSource final : public render::AudioSource {
public:
    void define(model::MediaRefId media, float value, std::int32_t channels = 2) {
        values_[media.value()] = {value, channels};
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
            for (std::int64_t i = 0; i < produced; ++i) {
                samples[i] = found->second.value;
            }
        }
        return {};
    }

    time::RationalTime lastSourceStart;

private:
    struct Entry {
        float value;
        std::int32_t channels;
    };
    std::map<std::uint64_t, Entry> values_;
    std::int64_t available_{-1};
};

}  // namespace zaro::testing
