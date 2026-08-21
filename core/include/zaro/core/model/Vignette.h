#pragma once

namespace zaro::model {

/// Darken (or lift) the frame towards its corners.
///
/// **In output coordinates, like the mask**, and for the same reason: where the
/// darkening sits is a fact about the frame, not about the picture being drawn
/// into it, so it stays put when the clip moves or scales. `Mask` already
/// observed that a vignette and a spotlight are the same shape with the
/// inversion flipped; what differs here is what the shape multiplies. A mask
/// changes coverage, so it makes pixels *transparent*; a vignette changes
/// brightness, so what is underneath does not show through the corners.
struct Vignette {
    /// How far the corners move. Negative darkens, which is what the word
    /// usually means; positive lifts them, which is occasionally what somebody
    /// wants and costs nothing to allow.
    double amount{0.0};

    /// Where the falloff begins, as a distance from the centre: 0 is the
    /// centre, 1 is the middle of an edge, and about 1.41 is a corner. So a
    /// midpoint above 1.41 darkens nothing at all.
    double midpoint{0.6};

    /// How far the falloff takes to finish, in the same units.
    double feather{0.5};

    /// 1 follows the frame's shape, so a widescreen frame gets a widescreen
    /// oval; 0 is a circle in pixels, which is what a lens actually does.
    double roundness{1.0};

    [[nodiscard]] bool isSet() const noexcept { return amount != 0.0; }

    friend bool operator==(const Vignette&, const Vignette&) = default;
};

}  // namespace zaro::model
