#pragma once

#include <cstdint>

namespace zaro::model {

/// What a keyer is looking for.
enum class KeyKind : std::uint8_t {
    None,
    /// A colour: everything close enough to it becomes transparent. A green or
    /// blue screen, which is what a key almost always is.
    Chroma,
    /// A brightness range: everything inside it becomes transparent. For
    /// footage shot against black, and for matte passes that arrive as
    /// greyscale.
    Luma,
};

/// Make part of a clip transparent, so what is under it shows through.
///
/// **Distance from a colour, not a region of colour space.** This looks like
/// the HSL qualifier and is not the same question. A qualifier describes an
/// area -- these hues, this much saturation, these tones -- because grading a
/// *kind* of thing is what secondaries are for. A key starts with an
/// eyedropper on one pixel of a screen, and "everything within this much of
/// that" is what somebody means by it. Expressing that as three windows would
/// mean turning one number into six, and would give a box in colour space
/// where a ball is wanted -- so a green screen with a warm corner keys out
/// either too much or too little depending which face of the box it crosses.
struct Keyer {
    KeyKind kind{KeyKind::None};

    /// The colour to key, in display terms: what an eyedropper on the picture
    /// reports. Converted to linear once, on the CPU, like every other
    /// threshold somebody types.
    double red{0.0};
    double green{1.0};
    double blue{0.0};

    /// How far from the key colour still counts as background, and how far
    /// beyond that the edge fades out.
    ///
    /// Measured in chromaticity -- the colour with its brightness divided out
    /// -- so a shadow on the screen is the same colour as the lit part of it.
    /// A distance measured on the raw values instead would key the top of an
    /// unevenly lit screen and leave the bottom, which is every screen.
    double tolerance{0.12};
    double softness{0.06};

    /// The luma key's window, display-referred, with the same soft edges the
    /// qualifier uses.
    double lumaLow{0.0};
    double lumaHigh{0.1};
    double lumaSoftness{0.05};

    /// How much of the key colour to pull out of what survives, 0 to 1.
    ///
    /// A key removes the screen; it does not remove the green bouncing off it
    /// onto somebody's shoulder. Suppression is what makes the difference
    /// between a cut-out and a composite, and leaving it at zero by default
    /// would mean every first attempt looked wrong for a reason nobody could
    /// see.
    double spill{1.0};

    /// Show the matte instead of the picture.
    ///
    /// The same reason the qualifier has one: judging a key by looking at the
    /// composite is guesswork, and the holes that matter are the ones too faint
    /// to see against whatever happens to be underneath.
    bool showMatte{false};

    [[nodiscard]] bool isSet() const noexcept { return kind != KeyKind::None; }

    friend bool operator==(const Keyer&, const Keyer&) = default;
};

}  // namespace zaro::model
