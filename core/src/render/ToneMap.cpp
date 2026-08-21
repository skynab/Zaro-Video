#include "zaro/core/render/ToneMap.h"

#include <algorithm>
#include <cmath>

namespace zaro::render {

float rolloff(float linear, float knee) {
    if (knee >= 1.0F || linear <= knee) {
        return linear;
    }
    // Above the knee, the remaining headroom is spent asymptotically. At the
    // knee this is exactly `knee` with slope exactly 1, so it joins the
    // identity without a corner; as the input grows it approaches 1 and never
    // reaches it, so two different highlights never come out the same value.
    const float headroom = 1.0F - knee;
    if (headroom <= 0.0F) {
        return knee;
    }
    return knee + (headroom * (1.0F - std::exp(-(linear - knee) / headroom)));
}

void toneMap(RgbaImage& image, float knee) {
    if (!image.isValid() || knee >= 1.0F) {
        return;
    }
    for (std::int32_t y = 0; y < image.height(); ++y) {
        Rgba* row = image.row(y);
        for (std::int32_t x = 0; x < image.width(); ++x) {
            Rgba& pixel = row[x];
            if (pixel.a <= 0.0001F) {
                continue;
            }
            const float inverse = 1.0F / pixel.a;
            const float r = rolloff(pixel.r * inverse, knee);
            const float g = rolloff(pixel.g * inverse, knee);
            const float b = rolloff(pixel.b * inverse, knee);
            pixel.r = r * pixel.a;
            pixel.g = g * pixel.a;
            pixel.b = b * pixel.a;
        }
    }
}

}  // namespace zaro::render
