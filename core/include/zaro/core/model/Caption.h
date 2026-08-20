#pragma once

#include <string>
#include <vector>

#include "zaro/core/time/TimeRange.h"

namespace zaro::model {

/// One caption, on screen for a span of time.
///
/// **Timed in milliseconds, not frames.** Subtitle formats are defined in
/// milliseconds, and a caption imported at 24fps, rounded to frames, and
/// written back out would come back a few milliseconds different from the file
/// that produced it — every time, compounding across a round trip through
/// another tool. The rate is 1000 and the arithmetic stays exact.
struct Caption {
    time::TimeRange range;
    /// Newlines separate the lines a caption shows. Kept as text rather than a
    /// list, because that is what every subtitle format stores and splitting it
    /// on the way in only to rejoin it on the way out invents differences.
    std::string text;

    friend bool operator==(const Caption&, const Caption&) = default;
};

/// How captions are drawn when they are burned in.
///
/// One style for all of them: a caption track is a stream of text with a single
/// look, and per-cue styling is what a graphics template is for.
struct CaptionStyle {
    std::string family;
    double pointSize{48.0};
    bool bold{false};
    /// Distance from the bottom of the frame to the bottom of the text box, in
    /// output pixels.
    double bottomMargin{80.0};
    /// Fraction of the frame width the text box occupies.
    double widthFraction{0.8};

    double red{1.0};
    double green{1.0};
    double blue{1.0};
    double alpha{1.0};

    friend bool operator==(const CaptionStyle&, const CaptionStyle&) = default;
};

/// A sequence's captions.
class CaptionTrack {
public:
    [[nodiscard]] bool empty() const noexcept { return captions_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return captions_.size(); }
    [[nodiscard]] const std::vector<Caption>& captions() const noexcept { return captions_; }
    [[nodiscard]] const CaptionStyle& style() const noexcept { return style_; }
    void setStyle(const CaptionStyle& style) { style_ = style; }

    /// Whether captions are drawn into the picture.
    ///
    /// Off by default: a caption file delivered alongside the picture is the
    /// normal case, and burning in is a decision with no way back once the file
    /// is written.
    [[nodiscard]] bool isBurnedIn() const noexcept { return burnIn_; }
    void setBurnedIn(bool value) noexcept { burnIn_ = value; }

    /// Add a caption, keeping the list ordered by start time.
    void add(const Caption& caption);
    void clear() { captions_.clear(); }
    bool removeAt(std::size_t index);

    /// The captions showing at a moment.
    ///
    /// Plural: formats allow overlapping cues, and a reader that assumed one
    /// would drop the second half of every conversation where two people speak
    /// over each other.
    [[nodiscard]] std::vector<const Caption*> at(const time::RationalTime& when) const;

    friend bool operator==(const CaptionTrack&, const CaptionTrack&) = default;

private:
    std::vector<Caption> captions_;
    CaptionStyle style_;
    bool burnIn_{false};
};

}  // namespace zaro::model
