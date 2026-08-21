#pragma once

#include <cmath>
#include <string>

namespace zaro::model {

/// Primary colour correction for one clip.
///
/// **Applied in scene-linear light**, which is the whole payoff of
/// [ADR-005](docs/adr/0005). Exposure is then a multiply in stops and behaves
/// the way a stop does on a camera; white balance is a set of channel gains
/// rather than a curve-shaped approximation of one; and none of it has to
/// undo a transfer function first.
///
/// The units are the ones a panel shows, not the ones the arithmetic wants:
/// exposure in stops, everything else on a -100..100 scale where 0 is neutral,
/// except saturation where 100 is neutral. Converting at the edge keeps the
/// stored numbers the ones somebody typed.
struct ColorCorrection {
    /// Warmer above zero, cooler below.
    double temperature{0.0};
    /// Magenta above zero, green below.
    double tint{0.0};
    /// Stops. Exact powers of two, so +1 is twice the light.
    double exposure{0.0};
    double contrast{0.0};
    /// 100 is untouched, 0 is monochrome, 200 is twice as saturated.
    double saturation{100.0};

    [[nodiscard]] bool isIdentity() const noexcept {
        return temperature == 0.0 && tint == 0.0 && exposure == 0.0 && contrast == 0.0 &&
               saturation == 100.0;
    }

    friend bool operator==(const ColorCorrection&, const ColorCorrection&) = default;
};

/// The three colour wheels: shadows, midtones and highlights.
///
/// **This is an ASC CDL**, and deliberately so. Every grading tool has three
/// wheels and almost none of them mean quite the same arithmetic by it; the CDL
/// is the one definition that other programs agree on, which means a grade set
/// here can be handed to somebody else and land the same way. It also means the
/// numbers can be written to and read from a .cdl or an EDL later without
/// having to be reinterpreted.
///
/// Per channel: `out = (in * slope + offset) ^ power`.
///   - `slope` scales, so it moves the highlights most and leaves black alone.
///   - `offset` adds, so it moves the shadows most and lifts black off zero.
///   - `power` is a gamma, so it moves the midtones and pins both ends.
///
/// That is what makes three wheels feel like shadows, midtones and highlights
/// even though every one of them touches the whole picture.
struct ColorWheels {
    double slopeR{1.0};
    double slopeG{1.0};
    double slopeB{1.0};

    double offsetR{0.0};
    double offsetG{0.0};
    double offsetB{0.0};

    double powerR{1.0};
    double powerG{1.0};
    double powerB{1.0};

    [[nodiscard]] bool isIdentity() const noexcept {
        return slopeR == 1.0 && slopeG == 1.0 && slopeB == 1.0 && offsetR == 0.0 &&
               offsetG == 0.0 && offsetB == 0.0 && powerR == 1.0 && powerG == 1.0 && powerB == 1.0;
    }

    friend bool operator==(const ColorWheels&, const ColorWheels&) = default;
};

/// A look LUT, by path.
///
/// The path rather than the contents: a .cube is a few hundred kilobytes of
/// text, and a project file that inlined one per clip would be unopenable in a
/// text editor and unmergeable in version control. The cost is that a project
/// can be moved away from its LUTs, which is the same bargain the media
/// references already make.
struct LutRef {
    std::string path;
    /// How much of the look to apply, 0 to 1. Every grading tool has this
    /// because a look at full strength is rarely the one anybody ships.
    double amount{1.0};

    [[nodiscard]] bool isSet() const noexcept { return !path.empty() && amount > 0.0; }

    friend bool operator==(const LutRef&, const LutRef&) = default;
};

}  // namespace zaro::model
