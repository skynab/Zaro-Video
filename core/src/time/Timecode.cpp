#include "zaro/core/time/Timecode.h"

#include <array>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>

namespace zaro::time {
namespace {

constexpr std::int64_t kSecondsPerDay = 24 * 60 * 60;

/// Frame labels per minute and per ten minutes under drop frame. Nine of every
/// ten minutes lose `drop` labels; the tenth keeps all of them.
struct DropGeometry {
    std::int64_t nominal;
    std::int64_t drop;
    std::int64_t perMinute;
    std::int64_t perTenMinutes;
};

DropGeometry geometryFor(const Rational& rate, bool dropFrame) {
    const std::int64_t nominal = nominalRate(rate);
    const std::int64_t drop = dropFrame ? dropFrameCount(rate) : 0;
    return DropGeometry{nominal, drop, nominal * 60 - drop, nominal * 600 - drop * 9};
}

std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

std::int64_t floorMod(std::int64_t a, std::int64_t b) {
    return a - floorDiv(a, b) * b;
}

}  // namespace

bool supportsDropFrame(const Rational& rate) {
    return rate.den() == 1001 && rate.num() > 0 && rate.num() % 30000 == 0;
}

std::int64_t dropFrameCount(const Rational& rate) {
    if (!supportsDropFrame(rate)) {
        return 0;
    }
    return 2 * (nominalRate(rate) / 30);
}

std::int64_t framesPerTimecodeDay(const Rational& rate, bool dropFrame) {
    const DropGeometry g = geometryFor(rate, dropFrame && supportsDropFrame(rate));
    return g.nominal * kSecondsPerDay - g.drop * 9 * 144;
}

bool isValidTimecode(const Timecode& tc, const Rational& rate) {
    const std::int64_t nominal = nominalRate(rate);
    if (tc.hours < 0 || tc.hours > 23 || tc.minutes < 0 || tc.minutes > 59 || tc.seconds < 0 ||
        tc.seconds > 59 || tc.frames < 0 || tc.frames >= nominal) {
        return false;
    }
    if (!tc.dropFrame) {
        return true;
    }
    if (!supportsDropFrame(rate)) {
        return false;
    }
    // The skipped labels: the first `drop` frames of every minute that is not a
    // multiple of ten simply do not exist.
    const auto drop = static_cast<std::int32_t>(dropFrameCount(rate));
    return !(tc.seconds == 0 && tc.minutes % 10 != 0 && tc.frames < drop);
}

Timecode timecodeFromFrames(std::int64_t frame, const Rational& rate, bool dropFrame) {
    dropFrame = dropFrame && supportsDropFrame(rate);
    assert(nominalRate(rate) > 0 && "timecode needs a positive rate");

    const bool negative = frame < 0;
    std::int64_t index = negative ? -frame : frame;
    index = floorMod(index, framesPerTimecodeDay(rate, dropFrame));

    const DropGeometry g = geometryFor(rate, dropFrame);

    // Convert the drop-frame count into the equivalent straight count by adding
    // back the labels that were skipped, then read the fields off normally.
    std::int64_t labelled = index;
    if (dropFrame) {
        const std::int64_t tenMinuteBlocks = index / g.perTenMinutes;
        const std::int64_t withinBlock = index % g.perTenMinutes;
        labelled += g.drop * 9 * tenMinuteBlocks;
        if (withinBlock >= g.drop) {
            labelled += g.drop * ((withinBlock - g.drop) / g.perMinute);
        }
    }

    Timecode tc;
    tc.dropFrame = dropFrame;
    tc.negative = negative && frame != 0;
    tc.frames = static_cast<std::int32_t>(labelled % g.nominal);
    tc.seconds = static_cast<std::int32_t>((labelled / g.nominal) % 60);
    tc.minutes = static_cast<std::int32_t>((labelled / (g.nominal * 60)) % 60);
    tc.hours = static_cast<std::int32_t>((labelled / (g.nominal * 3600)) % 24);
    return tc;
}

Timecode timecodeFromTime(const RationalTime& t, bool dropFrame) {
    return timecodeFromFrames(t.frames(), t.rate(), dropFrame);
}

std::optional<std::int64_t> framesFromTimecode(const Timecode& tc, const Rational& rate) {
    if (!isValidTimecode(tc, rate)) {
        return std::nullopt;
    }
    const DropGeometry g = geometryFor(rate, tc.dropFrame);

    std::int64_t frame = g.nominal * 3600 * tc.hours + g.nominal * 60 * tc.minutes +
                         g.nominal * tc.seconds + tc.frames;
    if (tc.dropFrame) {
        const std::int64_t totalMinutes = 60LL * tc.hours + tc.minutes;
        frame -= g.drop * (totalMinutes - totalMinutes / 10);
    }
    return tc.negative ? -frame : frame;
}

std::optional<RationalTime> timeFromTimecode(const Timecode& tc, const Rational& rate) {
    const auto frame = framesFromTimecode(tc, rate);
    if (!frame) {
        return std::nullopt;
    }
    return RationalTime{*frame, rate};
}

std::string Timecode::toString() const {
    const char last = dropFrame ? ';' : ':';
    std::array<char, 32> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%s%02d:%02d:%02d%c%02d",
                                      negative ? "-" : "", hours, minutes, seconds, last, frames);
    if (written <= 0) {
        return {};
    }
    return std::string{buffer.data(), static_cast<std::size_t>(written)};
}

std::optional<Timecode> parseTimecode(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }

    bool negative = false;
    if (text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    } else if (text.front() == '+') {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }

    // Fields are read right to left so short input fills frames first, which is
    // what a timecode entry field needs: "12" means twelve frames.
    std::array<std::int32_t, 4> fields{0, 0, 0, 0};
    std::size_t fieldCount = 0;
    bool dropFrame = false;
    std::int32_t value = 0;
    std::int32_t digitScale = 1;
    bool sawDigit = false;

    const auto pushField = [&]() -> bool {
        if (fieldCount >= fields.size() || !sawDigit) {
            return false;
        }
        fields[fieldCount++] = value;
        value = 0;
        digitScale = 1;
        sawDigit = false;
        return true;
    };

    for (auto it = text.rbegin(); it != text.rend(); ++it) {
        const char c = *it;
        if (c == ':' || c == ';' || c == '.') {
            if (c == ';') {
                dropFrame = true;
            }
            if (!pushField()) {
                return std::nullopt;
            }
        } else if (c >= '0' && c <= '9') {
            // Scanning right to left, so digits arrive least-significant first
            // and the place value has to be carried explicitly. Deriving it
            // from `value` instead would mis-handle a leading zero, e.g. "10".
            if (digitScale > 10000) {
                return std::nullopt;
            }
            value += static_cast<std::int32_t>(c - '0') * digitScale;
            digitScale *= 10;
            sawDigit = true;
        } else {
            return std::nullopt;
        }
    }
    if (!pushField()) {
        return std::nullopt;
    }

    Timecode tc;
    tc.frames = fields[0];
    tc.seconds = fields[1];
    tc.minutes = fields[2];
    tc.hours = fields[3];
    tc.dropFrame = dropFrame;
    tc.negative = negative;
    return tc;
}

std::optional<std::int64_t> framesFromTimecodeString(std::string_view text, const Rational& rate) {
    const auto tc = parseTimecode(text);
    if (!tc) {
        return std::nullopt;
    }
    return framesFromTimecode(*tc, rate);
}

}  // namespace zaro::time
