#pragma once

#include <cstdint>

namespace zaro::model {

/// How a clip is placed in the frame.
///
/// These are geometry, not time, so they are doubles rather than rationals.
/// A position is a continuous quantity that never has to be exact for two
/// values to line up; a frame boundary does, which is why time is rational and
/// this is not.
///
/// Coordinates are in output pixels with the origin at the centre of the frame,
/// which is what makes scale and rotation behave the way people expect without
/// having to think about the frame size.
struct Transform {
    double positionX{0.0};
    double positionY{0.0};
    double scaleX{1.0};
    double scaleY{1.0};
    double rotationDegrees{0.0};
    /// The point the clip scales and rotates about, in source pixels relative
    /// to the source centre.
    double anchorX{0.0};
    double anchorY{0.0};
    double opacity{1.0};

    [[nodiscard]] bool isIdentity() const noexcept {
        return positionX == 0.0 && positionY == 0.0 && scaleX == 1.0 && scaleY == 1.0 &&
               rotationDegrees == 0.0 && anchorX == 0.0 && anchorY == 0.0 && opacity == 1.0;
    }

    friend bool operator==(const Transform&, const Transform&) = default;
};

enum class BlendMode : std::uint8_t { Normal, Add, Multiply, Screen };

[[nodiscard]] const char* toString(BlendMode mode) noexcept;
[[nodiscard]] BlendMode blendModeFromString(const char* name) noexcept;

}  // namespace zaro::model
