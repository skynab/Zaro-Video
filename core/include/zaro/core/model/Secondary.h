#pragma once

#include "zaro/core/model/ColorCorrection.h"

namespace zaro::model {

/// Which pixels a secondary correction applies to.
///
/// Three windows — hue, saturation, luma — multiplied together. A pixel is in
/// the selection to the extent that it is inside all three.
///
/// Every edge is soft. A hard threshold on a qualifier produces a mask with
/// stepped edges, and a correction applied through it looks like a sticker
/// rather than a grade; the softness is the difference between a secondary you
/// can use and one you can only demonstrate.
struct HslQualifier {
    bool enabled{false};

    /// Degrees. The window wraps, because hue does: a selection centred on red
    /// runs from about 350 to about 10 and would otherwise be empty.
    double hueCentre{0.0};
    double hueWidth{360.0};
    double hueSoftness{15.0};

    /// 0..1. Saturation is `(max - min) / max` of the linear channels.
    double saturationLow{0.0};
    double saturationHigh{1.0};
    double saturationSoftness{0.05};

    /// 0..1, in *display* terms — "midtones" means the tones that look like
    /// midtones, not the linear values halfway to white. The thresholds are
    /// converted to linear once, on the CPU, so nothing downstream needs a
    /// transfer function to compare against them.
    double lumaLow{0.0};
    double lumaHigh{1.0};
    double lumaSoftness{0.05};

    friend bool operator==(const HslQualifier&, const HslQualifier&) = default;
};

/// A correction applied only where the qualifier selects.
struct Secondary {
    HslQualifier qualifier;
    ColorCorrection correction;

    /// Show the selection as a greyscale mask instead of the picture.
    ///
    /// Not a debugging aid: judging a qualifier by looking at the corrected
    /// result is guesswork, and every grading tool has this because it is the
    /// only way to see what is actually selected.
    bool showMask{false};

    [[nodiscard]] bool isActive() const {
        return qualifier.enabled && (showMask || !correction.isIdentity());
    }

    friend bool operator==(const Secondary&, const Secondary&) = default;
};

}  // namespace zaro::model
