#include "zaro/core/render/EffectStack.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace zaro::render {
namespace {

/// A normalised Gaussian, truncated where it stops mattering.
///
/// Three standard deviations either side holds 99.7% of the weight. Going
/// further costs samples for a difference below the noise floor of any real
/// footage; stopping earlier leaves a visible edge on the kernel, which reads
/// as a faint box around bright objects.
std::vector<float> kernelFor(float radius) {
    const auto reach = std::max(1, static_cast<int>(std::ceil(radius * 3.0F)));
    std::vector<float> weights(static_cast<std::size_t>((2 * reach) + 1));

    const float denominator = 2.0F * radius * radius;
    float total = 0.0F;
    for (int i = -reach; i <= reach; ++i) {
        const auto distance = static_cast<float>(i);
        const float weight = std::exp(-(distance * distance) / denominator);
        weights[static_cast<std::size_t>(i + reach)] = weight;
        total += weight;
    }
    // Normalised so the filter preserves brightness. An unnormalised kernel
    // darkens or brightens the picture in proportion to the radius, which looks
    // like an exposure bug rather than a blur one.
    const float inverse = 1.0F / total;
    for (float& weight : weights) {
        weight *= inverse;
    }
    return weights;
}

/// One pass along one axis. Edges clamp: the alternative is treating outside
/// the frame as transparent, which darkens every border by half the kernel.
void blurAxis(const RgbaImage& in, RgbaImage& out, const std::vector<float>& weights,
              bool horizontal) {
    const auto reach = static_cast<int>(weights.size() / 2);
    const std::int32_t width = in.width();
    const std::int32_t height = in.height();

    for (std::int32_t y = 0; y < height; ++y) {
        Rgba* target = out.row(y);
        for (std::int32_t x = 0; x < width; ++x) {
            float r = 0.0F;
            float g = 0.0F;
            float b = 0.0F;
            float a = 0.0F;
            for (int i = -reach; i <= reach; ++i) {
                const float weight = weights[static_cast<std::size_t>(i + reach)];
                std::int32_t sampleX = x;
                std::int32_t sampleY = y;
                if (horizontal) {
                    sampleX = std::clamp(x + i, 0, width - 1);
                } else {
                    sampleY = std::clamp(y + i, 0, height - 1);
                }
                const Rgba& pixel = in.row(sampleY)[sampleX];
                r += pixel.r * weight;
                g += pixel.g * weight;
                b += pixel.b * weight;
                a += pixel.a * weight;
            }
            target[x] = Rgba{r, g, b, a};
        }
    }
}

void fit(RgbaImage& buffer, const RgbaImage& like) {
    if (buffer.width() != like.width() || buffer.height() != like.height()) {
        buffer = RgbaImage{like.width(), like.height()};
    }
}

}  // namespace

void blur(RgbaImage& image, RgbaImage& scratch, float radius) {
    if (!image.isValid() || radius <= 0.0F) {
        return;
    }
    fit(scratch, image);
    const std::vector<float> weights = kernelFor(radius);
    blurAxis(image, scratch, weights, true);
    blurAxis(scratch, image, weights, false);
}

void applyEffects(const std::vector<model::Effect>& effects, RgbaImage& image, RgbaImage& scratch,
                  RgbaImage& scratch2, double seconds) {
    if (!image.isValid()) {
        return;
    }
    for (const model::Effect& effect : effects) {
        if (!effect.enabled) {
            continue;
        }
        const auto radius = static_cast<float>(effect.valueAt(model::EffectParam::Radius, seconds));
        switch (effect.kind) {
            case model::EffectKind::Blur:
                blur(image, scratch, radius);
                break;
            case model::EffectKind::Sharpen: {
                const auto amount =
                    static_cast<float>(effect.valueAt(model::EffectParam::Amount, seconds));
                if (amount <= 0.0F || radius <= 0.0F) {
                    break;
                }
                // Unsharp masking: what a sharpen is, everywhere. The detail is
                // whatever the blur threw away, and adding it back exaggerates
                // exactly the edges the blur softened.
                scratch2 = image.clone();
                blur(scratch2, scratch, radius);

                for (std::int32_t y = 0; y < image.height(); ++y) {
                    Rgba* row = image.row(y);
                    const Rgba* soft = scratch2.row(y);
                    for (std::int32_t x = 0; x < image.width(); ++x) {
                        Rgba& pixel = row[x];
                        pixel.r += (pixel.r - soft[x].r) * amount;
                        pixel.g += (pixel.g - soft[x].g) * amount;
                        pixel.b += (pixel.b - soft[x].b) * amount;
                        // Alpha is left alone. Sharpening coverage would put a
                        // bright rim along every edge of a keyed subject, which
                        // is the artefact people blame the key for.
                        //
                        // Negative light is not a thing, so an overshoot into
                        // the shadows clamps at black rather than inverting.
                        pixel.r = std::max(pixel.r, 0.0F);
                        pixel.g = std::max(pixel.g, 0.0F);
                        pixel.b = std::max(pixel.b, 0.0F);
                    }
                }
                break;
            }
        }
    }
}

}  // namespace zaro::render
