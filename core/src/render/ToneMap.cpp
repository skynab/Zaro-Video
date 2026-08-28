#include "zaro/core/render/ToneMap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace zaro::render {

float rolloff(float linear, float knee) {
    if (knee >= 1.0F || linear <= knee) {
        return linear;
    }
    const float headroom = 1.0F - knee;
    if (headroom <= 0.0F) {
        return knee;
    }
    // Above the knee the remaining headroom is spent asymptotically. At the
    // knee this is exactly `knee` with slope exactly 1, so it joins the
    // identity without a corner; as the input grows it approaches 1 without
    // reaching it, so two different highlights never come out the same value.
    //
    // Rational rather than exponential, and that is not a matter of taste. An
    // exponential rolloff underflows to exactly 1 about four and a half stops
    // above the knee, which sounds far away until you remember that PQ arrives
    // with values up to 100 (Phase 6h): the top two stops of an HDR signal
    // would have collapsed to flat white. This form stays distinct until the
    // input is millions of times the headroom, which no signal is.
    const float above = linear - knee;
    return knee + (headroom * (above / (above + headroom)));
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
