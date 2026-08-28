#include "zaro/core/render/ShotMatch.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace zaro::render {
namespace {

/// The three points a match is built on, per channel.
///
/// A low percentile rather than the minimum, and a high one rather than the
/// maximum: one clipped highlight or one dead pixel would otherwise decide the
/// whole correction.
constexpr double kShadow = 0.10;
constexpr double kMid = 0.50;
constexpr double kHighlight = 0.90;

struct Anchors {
    std::array<double, 3> shadow{};
    std::array<double, 3> mid{};
    std::array<double, 3> highlight{};
};

double percentile(std::vector<float>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto at = static_cast<std::size_t>(fraction * static_cast<double>(sorted.size() - 1));
    return static_cast<double>(sorted[std::min(at, sorted.size() - 1)]);
}

Anchors anchorsOf(const RgbaImage& image) {
    Anchors out;
    std::array<std::vector<float>, 3> samples;
    // Every fourth pixel each way. A percentile is a summary; sixteen times
    // fewer samples moves it by less than the match cares about and makes
    // measuring a 4K frame something that happens while a button is still down.
    constexpr std::int32_t kStep = 4;
    for (std::int32_t y = 0; y < image.height(); y += kStep) {
        const Rgba* row = image.row(y);
        for (std::int32_t x = 0; x < image.width(); x += kStep) {
            const Rgba& pixel = row[x];
            if (pixel.a <= 0.0001F) {
                // Nothing there. A transparent pixel is not black, and counting
                // it as one would drag every shadow anchor to zero.
                continue;
            }
            const float inverse = 1.0F / pixel.a;
            samples[0].push_back(pixel.r * inverse);
            samples[1].push_back(pixel.g * inverse);
            samples[2].push_back(pixel.b * inverse);
        }
    }
    for (std::size_t channel = 0; channel < 3; ++channel) {
        std::sort(samples[channel].begin(), samples[channel].end());
        out.shadow[channel] = percentile(samples[channel], kShadow);
        out.mid[channel] = percentile(samples[channel], kMid);
        out.highlight[channel] = percentile(samples[channel], kHighlight);
    }
    return out;
}

double distanceBetween(const Anchors& a, const Anchors& b) {
    double sum = 0.0;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        sum += std::fabs(a.shadow[channel] - b.shadow[channel]);
        sum += std::fabs(a.mid[channel] - b.mid[channel]);
        sum += std::fabs(a.highlight[channel] - b.highlight[channel]);
    }
    return sum / 9.0;
}

}  // namespace

Result<ShotMatch> matchShot(const RgbaImage& reference, const RgbaImage& target) {
    if (!reference.isValid() || !target.isValid()) {
        return Error{ErrorCode::InvalidData, "there are not two frames to compare"};
    }

    const Anchors want = anchorsOf(reference);
    const Anchors have = anchorsOf(target);

    ShotMatch match;
    match.before = distanceBetween(want, have);
    int matched = 0;

    for (std::size_t channel = 0; channel < 3; ++channel) {
        // Slope and offset from the two ends. This is exact: the shadow lands
        // on the shadow and the highlight on the highlight, whatever the
        // midtone does.
        const double spread = have.highlight[channel] - have.shadow[channel];
        if (spread <= 1e-5) {
            // A channel with no range in it -- a blown-out frame, or a solid
            // colour. There is nothing to stretch, so it is left alone rather
            // than divided by nothing.
            continue;
        }
        ++matched;
        const double slope = (want.highlight[channel] - want.shadow[channel]) / spread;
        const double offset = want.shadow[channel] - (slope * have.shadow[channel]);

        // Then the power, from what is left of the midtone once the ends are in
        // place. Both sides have to be positive for a logarithm to mean
        // anything; a midtone at zero is a frame with no midtones.
        const double moved = (slope * have.mid[channel]) + offset;
        double power = 1.0;
        if (moved > 1e-4 && want.mid[channel] > 1e-4) {
            power = std::log(want.mid[channel]) / std::log(moved);
            // A wild exponent is a sign the two shots disagree about what a
            // midtone is, not a correction worth applying. Clamped to something
            // a person might have dialled.
            power = std::clamp(power, 0.25, 4.0);
        }

        switch (channel) {
            case 0:
                match.wheels.slopeR = slope;
                match.wheels.offsetR = offset;
                match.wheels.powerR = power;
                break;
            case 1:
                match.wheels.slopeG = slope;
                match.wheels.offsetG = offset;
                match.wheels.powerG = power;
                break;
            default:
                match.wheels.slopeB = slope;
                match.wheels.offsetB = offset;
                match.wheels.powerB = power;
                break;
        }
    }

    // What the correction actually achieves, measured rather than assumed: run
    // the target's own anchors through it and see where they land.
    Anchors after = have;
    const double slopes[3] = {match.wheels.slopeR, match.wheels.slopeG, match.wheels.slopeB};
    const double offsets[3] = {match.wheels.offsetR, match.wheels.offsetG, match.wheels.offsetB};
    const double powers[3] = {match.wheels.powerR, match.wheels.powerG, match.wheels.powerB};
    const auto apply = [&](double value, std::size_t channel) {
        const double scaled = (value * slopes[channel]) + offsets[channel];
        return scaled > 0.0 ? std::pow(scaled, powers[channel]) : 0.0;
    };
    for (std::size_t channel = 0; channel < 3; ++channel) {
        after.shadow[channel] = apply(have.shadow[channel], channel);
        after.mid[channel] = apply(have.mid[channel], channel);
        after.highlight[channel] = apply(have.highlight[channel], channel);
    }
    match.after = distanceBetween(want, after);

    if (matched == 0) {
        // Every channel was flat. The two frames may be miles apart and there
        // is still nothing here to match *on* -- a correction built from three
        // anchors needs three distinguishable anchors. Reporting this as a
        // successful match of zero channels is the confidently-wrong answer
        // this whole design exists to avoid.
        match.reason = "there is no range in those frames to match on";
        return match;
    }

    // Usable when the correction is one somebody might have dialled. A match
    // that has to invent a huge slope is telling you the two shots are not of
    // the same thing, and applying it produces a confident answer to a question
    // nobody asked.
    const double widest = std::max({slopes[0], slopes[1], slopes[2]});
    const double narrowest = std::min({slopes[0], slopes[1], slopes[2]});
    if (narrowest <= 0.0 || widest > 8.0 || narrowest < 0.125) {
        match.reason = "those two shots are too unalike to match";
        return match;
    }
    if (match.after > match.before) {
        // It made things worse, which happens when the distributions cross.
        match.reason = "matching those two moves them further apart";
        return match;
    }
    match.usable = true;
    return match;
}

}  // namespace zaro::render
