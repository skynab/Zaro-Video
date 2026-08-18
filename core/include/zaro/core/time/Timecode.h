#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "zaro/core/time/Rational.h"
#include "zaro/core/time/RationalTime.h"

namespace zaro::time {

/// SMPTE timecode: the human-facing label for a frame, not a time.
///
/// Timecode counts in whole frames at a *nominal* rate -- 29.97fps material is
/// labelled as if it ran at 30. Over an hour that lie accumulates to 3.6
/// seconds, and drop-frame timecode is the correction: it skips two labels at
/// the top of every minute except every tenth minute, so that the label at
/// 01:00:00;00 lands within a couple of frames of a real elapsed hour.
///
/// Nothing is dropped from the media. Only the numbering skips.
struct Timecode {
    std::int32_t hours{0};
    std::int32_t minutes{0};
    std::int32_t seconds{0};
    std::int32_t frames{0};
    bool dropFrame{false};
    /// Timecodes before zero, which arise from relative offsets.
    bool negative{false};

    /// "01:00:00:00", or "00:59:59;29" when drop frame. The final separator is
    /// the drop-frame indicator, per SMPTE convention.
    [[nodiscard]] std::string toString() const;

    friend bool operator==(const Timecode&, const Timecode&) = default;
};

/// Drop frame is only defined for the 1000/1001-pulldown rates whose nominal
/// rate is a multiple of 30: 29.97, 59.94, 119.88. It is meaningless at 25 or
/// at true 30, and asking for it there is a bug worth catching early.
[[nodiscard]] bool supportsDropFrame(const Rational& rate);

/// How many labels are skipped at each affected minute boundary: 2 at 29.97,
/// 4 at 59.94, 8 at 119.88. Zero when the rate has no drop frame.
[[nodiscard]] std::int64_t dropFrameCount(const Rational& rate);

/// Number of distinct frame labels in a 24-hour timecode day, at which point
/// timecode wraps back to 00:00:00:00.
[[nodiscard]] std::int64_t framesPerTimecodeDay(const Rational& rate, bool dropFrame);

/// Whether the fields describe a label that exists at this rate. Catches both
/// out-of-range fields and the drop-frame labels that are skipped by
/// definition, such as 00:01:00;00 at 29.97.
[[nodiscard]] bool isValidTimecode(const Timecode& tc, const Rational& rate);

/// Frame index -> label. Wraps at 24 hours. Negative indices produce a
/// negative-flagged timecode of the corresponding magnitude.
[[nodiscard]] Timecode timecodeFromFrames(std::int64_t frame, const Rational& rate, bool dropFrame);
[[nodiscard]] Timecode timecodeFromTime(const RationalTime& t, bool dropFrame);

/// Label -> frame index. Returns nullopt for labels that do not exist at this
/// rate, so callers can reject bad user input rather than silently editing it.
[[nodiscard]] std::optional<std::int64_t> framesFromTimecode(const Timecode& tc,
                                                             const Rational& rate);
[[nodiscard]] std::optional<RationalTime> timeFromTimecode(const Timecode& tc,
                                                           const Rational& rate);

/// Parse "HH:MM:SS:FF". A semicolon anywhere marks drop frame. Fewer than four
/// fields fill from the right, so "12" is 12 frames and "5:00" is 5 seconds --
/// the shorthand editors expect from a timecode entry field. A leading '-'
/// gives a negative offset.
[[nodiscard]] std::optional<Timecode> parseTimecode(std::string_view text);

/// Convenience: parse straight to a frame index, validating against the rate.
[[nodiscard]] std::optional<std::int64_t> framesFromTimecodeString(std::string_view text,
                                                                   const Rational& rate);

}  // namespace zaro::time
