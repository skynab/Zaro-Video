#pragma once

#include <cmath>

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

}  // namespace zaro::model
