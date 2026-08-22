#include "zaro/core/render/Resample.h"

#include <algorithm>
#include <cmath>

namespace zaro::render {

void resizeInto(const RgbaImage& from, RgbaImage& to) {
    if (!from.isValid() || !to.isValid()) {
        return;
    }
    const double scaleX = static_cast<double>(from.width()) / static_cast<double>(to.width());
    const double scaleY = static_cast<double>(from.height()) / static_cast<double>(to.height());

    for (std::int32_t y = 0; y < to.height(); ++y) {
        Rgba* row = to.row(y);
        const double top = static_cast<double>(y) * scaleY;
        const double bottom = top + scaleY;
        for (std::int32_t x = 0; x < to.width(); ++x) {
            const double left = static_cast<double>(x) * scaleX;
            const double right = left + scaleX;

            const auto firstX = static_cast<std::int32_t>(std::floor(left));
            const auto lastX = static_cast<std::int32_t>(std::ceil(right)) - 1;
            const auto firstY = static_cast<std::int32_t>(std::floor(top));
            const auto lastY = static_cast<std::int32_t>(std::ceil(bottom)) - 1;
            if (lastX <= firstX && lastY <= firstY) {
                // One source pixel or fewer per output pixel: an enlargement,
                // where a box average is nearest-neighbour with extra steps.
                row[x] = from.sampleBilinear(static_cast<float>((left + right) / 2.0),
                                             static_cast<float>((top + bottom) / 2.0));
                continue;
            }

            double r = 0.0;
            double g = 0.0;
            double b = 0.0;
            double a = 0.0;
            double weight = 0.0;
            for (std::int32_t sy = std::max(0, firstY); sy <= std::min(from.height() - 1, lastY);
                 ++sy) {
                // How much of this row the output pixel covers, so an edge that
                // falls part way through a source pixel counts part way.
                const double coverY = std::min(bottom, static_cast<double>(sy) + 1.0) -
                                      std::max(top, static_cast<double>(sy));
                if (coverY <= 0.0) {
                    continue;
                }
                const Rgba* source = from.row(sy);
                for (std::int32_t sx = std::max(0, firstX); sx <= std::min(from.width() - 1, lastX);
                     ++sx) {
                    const double coverX = std::min(right, static_cast<double>(sx) + 1.0) -
                                          std::max(left, static_cast<double>(sx));
                    if (coverX <= 0.0) {
                        continue;
                    }
                    const double share = coverX * coverY;
                    r += static_cast<double>(source[sx].r) * share;
                    g += static_cast<double>(source[sx].g) * share;
                    b += static_cast<double>(source[sx].b) * share;
                    a += static_cast<double>(source[sx].a) * share;
                    weight += share;
                }
            }
            if (weight <= 0.0) {
                row[x] = Rgba{};
                continue;
            }
            row[x] = Rgba{static_cast<float>(r / weight), static_cast<float>(g / weight),
                          static_cast<float>(b / weight), static_cast<float>(a / weight)};
        }
    }
}

}  // namespace zaro::render
