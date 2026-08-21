#version 440

layout(location = 0) in vec2 texCoord;
layout(location = 1) in vec2 framePosition;

layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D source;

// Tone curves, baked on the CPU into linear-in, linear-out entries. The shader
// never sees a control point or a transfer function: it looks up what
// render::CurveTable already worked out, which is why the two paths cannot
// drift apart. See ADR-012.
layout(binding = 2) uniform sampler2D curveTable;

// A look LUT, baked on the CPU into a linear-in, linear-out cube on the same
// warped axis the curve table uses. The shader knows nothing about the .cube
// format, the LUT's domain, or transfer functions -- it samples what
// render::LutTable already worked out. See ADR-012.
layout(binding = 3) uniform sampler3D lutTable;

layout(std140, binding = 0) uniform Block {
    mat4 transform;
    vec4 params;  // x: opacity, z: frame width, w: frame height
    // Primary colour correction, precomputed on the CPU so this shader does
    // not re-derive what a temperature of -20 means. Two implementations of
    // that question are two answers, and preview would disagree with export by
    // an amount too small to notice and too large to accept.
    vec4 balance;  // rgb: white balance gains, w: exposure multiplier
    vec4 grade;    // x: contrast exponent, y: saturation, z: curves active
    // The secondary: a correction applied only where the qualifier selects.
    vec4 secBalance;  // rgb: white balance gains, w: exposure multiplier
    vec4 secGrade;    // x: contrast, y: saturation, z: show mask, w: enabled
    vec4 hueWindow;   // x: centre, y: inner, z: outer
    vec4 satWindow;   // x: innerLow, y: outerLow, z: innerHigh, w: outerHigh
    vec4 lumaWindow;  // x: innerLow, y: outerLow, z: innerHigh, w: outerHigh
    vec4 look;        // x: amount, y: axis maximum, z: active, w: cube size
    // A mask, in output pixels from the centre of the frame.
    vec4 maskBox;     // xy: half size, zw: centre
    vec4 maskEdge;    // x: corner radius, y: feather, z: shape (1 rect, 2 ellipse),
                      // w: inverted
    // The keyer: which pixels of this clip are transparent. Read before the
    // grade, on the colour the camera saw.
    vec4 keyColour;   // rgb: key chromaticity, w: kind (0 none, 1 chroma, 2 luma)
    vec4 keyEdge;     // x: tolerance, y: outer, z: spill amount, w: spill channel
    vec4 keyLuma;     // x: innerLow, y: outerLow, z: innerHigh, w: outerHigh
    vec4 keyFlags;    // x: show the matte
    // The display pass: fitting the finished frame into what the screen can
    // show. Only the present pass sets this; a composite draw leaves it at 1,
    // which is no rolloff at all.
    vec4 display;     // x: highlight knee
    // The three wheels, as an ASC CDL. Must agree with render::gradePixel.
    vec4 cdlSlope;
    vec4 cdlOffset;
    vec4 cdlPower;    // w: non-zero when the CDL is not the identity
} ubuf;



const float kMiddleGrey = 0.18;
const vec3 kLumaWeights = vec3(0.2126, 0.7152, 0.0722);

// Must agree with render::qualifierMask. It is checked against it.
float smoothly(float t)
{
    float c = clamp(t, 0.0, 1.0);
    return c * c * (3.0 - 2.0 * c);
}

float rampUp(float value, float outer, float inner)
{
    if (inner <= outer) {
        return value >= inner ? 1.0 : 0.0;
    }
    return smoothly((value - outer) / (inner - outer));
}

float rampDown(float value, float inner, float outer)
{
    if (outer <= inner) {
        return value <= inner ? 1.0 : 0.0;
    }
    return smoothly((outer - value) / (outer - inner));
}

float qualifierMask(vec3 colour)
{
    float high = max(colour.r, max(colour.g, colour.b));
    float low = min(colour.r, min(colour.g, colour.b));
    float range = high - low;
    float saturation = high > 0.0001 ? range / high : 0.0;

    float hue = 0.0;
    if (range > 0.0001) {
        if (high == colour.r) {
            hue = 60.0 * mod((colour.g - colour.b) / range, 6.0);
        } else if (high == colour.g) {
            hue = 60.0 * (((colour.b - colour.r) / range) + 2.0);
        } else {
            hue = 60.0 * (((colour.r - colour.g) / range) + 4.0);
        }
        if (hue < 0.0) {
            hue += 360.0;
        }
    }

    float luma = dot(colour, kLumaWeights);

    // The whole circle selects every hue, including the neutral pixels whose
    // hue is arbitrary.
    float distance = abs(hue - ubuf.hueWindow.x);
    if (distance > 180.0) {
        distance = 360.0 - distance;
    }
    float hueMask = ubuf.hueWindow.y >= 180.0
                        ? 1.0
                        : rampDown(distance, ubuf.hueWindow.y, ubuf.hueWindow.z);

    float satMask = rampUp(saturation, ubuf.satWindow.y, ubuf.satWindow.x) *
                    rampDown(saturation, ubuf.satWindow.z, ubuf.satWindow.w);
    float lumaMask = rampUp(luma, ubuf.lumaWindow.y, ubuf.lumaWindow.x) *
                     rampDown(luma, ubuf.lumaWindow.z, ubuf.lumaWindow.w);

    return clamp(hueMask * satMask * lumaMask, 0.0, 1.0);
}

// Must agree with render::gradePixel. It is checked against it.
vec3 applyGrade(vec3 colour)
{
    colour *= ubuf.balance.rgb * ubuf.balance.w;

    if (ubuf.cdlPower.w != 0.0) {
        // The three wheels, before contrast, for the reason render::gradePixel
        // gives. Negative light has no fractional power, so anything at or
        // below zero stops there rather than becoming a NaN.
        vec3 scaled = colour * ubuf.cdlSlope.rgb + ubuf.cdlOffset.rgb;
        colour = mix(vec3(0.0), pow(max(scaled, vec3(0.0)), ubuf.cdlPower.rgb),
                     step(vec3(1e-30), scaled));
    }

    if (ubuf.grade.x != 1.0) {
        // Non-positive light has no fractional power, and one NaN spreads
        // through everything it is averaged with. Left where it is, as on the
        // CPU.
        vec3 lifted = max(colour, vec3(0.0));
        vec3 curved = kMiddleGrey * pow(lifted / kMiddleGrey, vec3(ubuf.grade.x));
        colour = mix(colour, curved, step(vec3(1e-8), colour));
    }

    if (ubuf.grade.y != 1.0) {
        float grey = dot(colour, kLumaWeights);
        colour = vec3(grey) + (colour - vec3(grey)) * ubuf.grade.y;
    }

    if (ubuf.look.z != 0.0) {
        // The same index as the curve table, scaled into the cube's own axis
        // and clamped to it -- above the LUT's domain the answer is its edge.
        vec3 lifted = max(colour, vec3(0.0));
        vec3 index = clamp(sqrt(lifted / (vec3(1.0) + lifted)) / max(ubuf.look.y, 1e-4), 0.0, 1.0);
        // Texel centres. A sampler reads the middle of a texel, so a coordinate
        // of 0 lands half a texel outside the first entry and the whole cube is
        // read shifted -- which looks like a slightly wrong look rather than a
        // sampling mistake.
        float cubeSize = max(ubuf.look.w, 1.0);
        vec3 coord = ((index * (cubeSize - 1.0)) + 0.5) / cubeSize;
        vec3 warped = texture(lutTable, coord).rgb;
        // Un-warp: the cube stores indices, not light, so that an identity LUT
        // interpolates exactly.
        vec3 squared = warped * warped;
        vec3 looked = squared / max(vec3(1.0) - squared, vec3(1e-6));
        colour = mix(colour, looked, ubuf.look.x);
    }

    if (ubuf.grade.z != 0.0) {
        // Must be exactly render::CurveTable::indexFor. Three operations, so
        // there is nothing here to get subtly different.
        vec3 lifted = max(colour, vec3(0.0));
        vec3 index = sqrt(lifted / (vec3(1.0) + lifted));
        colour = vec3(texture(curveTable, vec2(index.r, 0.5)).r,
                      texture(curveTable, vec2(index.g, 0.5)).g,
                      texture(curveTable, vec2(index.b, 0.5)).b);
    }

    if (ubuf.secGrade.w != 0.0) {
        float mask = qualifierMask(colour);
        if (ubuf.secGrade.z != 0.0) {
            return vec3(mask);
        }
        vec3 keyed = colour * ubuf.secBalance.rgb * ubuf.secBalance.w;
        if (ubuf.secGrade.x != 1.0) {
            vec3 lifted = max(keyed, vec3(0.0));
            vec3 curved = kMiddleGrey * pow(lifted / kMiddleGrey, vec3(ubuf.secGrade.x));
            keyed = mix(keyed, curved, step(vec3(1e-8), keyed));
        }
        if (ubuf.secGrade.y != 1.0) {
            float grey = dot(keyed, kLumaWeights);
            keyed = vec3(grey) + (keyed - vec3(grey)) * ubuf.secGrade.y;
        }
        // Blended by the mask, not switched on it: the soft edge is the point.
        colour = mix(colour, keyed, mask);
    }
    return colour;
}

// Must agree with render::keyMatte. Below this total intensity a pixel has no
// reliable colour, and is kept: black in front of a green screen is a subject.
const float kIntensityFloor = 1e-4;

float keyMatte(vec3 colour)
{
    if (ubuf.keyColour.w > 1.5) {
        float luma = dot(colour, kLumaWeights);
        float inside = rampUp(luma, ubuf.keyLuma.y, ubuf.keyLuma.x) *
                       rampDown(luma, ubuf.keyLuma.z, ubuf.keyLuma.w);
        return clamp(1.0 - inside, 0.0, 1.0);
    }

    float sum = colour.r + colour.g + colour.b;
    if (sum <= kIntensityFloor) {
        return 1.0;
    }
    vec3 d = (colour / sum) - ubuf.keyColour.rgb;
    return rampUp(length(d), ubuf.keyEdge.x, ubuf.keyEdge.y);
}

// Must agree with render::suppressSpill.
vec3 suppressSpill(vec3 colour)
{
    if (ubuf.keyColour.w > 1.5 || ubuf.keyEdge.z <= 0.0) {
        return colour;
    }
    int index = int(ubuf.keyEdge.w + 0.5);
    float dominant = colour[index];
    float ceiling = (colour[(index + 1) % 3] + colour[(index + 2) % 3]) * 0.5;
    if (dominant <= ceiling) {
        return colour;
    }
    colour[index] = dominant + ((ceiling - dominant) * ubuf.keyEdge.z);
    return colour;
}

// Must agree with render::maskCoverage. The same signed distances the shape
// rasteriser uses, so a mask and a shape of the same size cover the same pixels.
float maskCoverage(vec2 offset)
{
    if (ubuf.maskEdge.z < 0.5) {
        return 1.0;
    }
    vec2 half_ = ubuf.maskBox.xy;
    if (half_.x <= 0.0 || half_.y <= 0.0) {
        return ubuf.maskEdge.w > 0.5 ? 1.0 : 0.0;
    }
    vec2 d = offset - ubuf.maskBox.zw;

    float distance;
    if (ubuf.maskEdge.z > 1.5) {
        vec2 n = d / half_;
        float value = length(n);
        if (value == 0.0) {
            distance = -min(half_.x, half_.y);
        } else {
            float gradient = length(n / half_);
            distance = (value - 1.0) / gradient;
        }
    } else {
        float r = clamp(ubuf.maskEdge.x, 0.0, min(half_.x, half_.y));
        vec2 q = abs(d) - (half_ - vec2(r));
        distance = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
    }

    float ramp = max(1.0, ubuf.maskEdge.y);
    float coverage = clamp(0.5 - (distance / ramp), 0.0, 1.0);
    return ubuf.maskEdge.w > 0.5 ? 1.0 - coverage : coverage;
}

// Must agree with render::rolloff. Exactly the identity at or below the knee,
// so a frame with nothing above white is presented untouched.
float rolloff(float v, float knee)
{
    if (knee >= 1.0 || v <= knee) {
        return v;
    }
    float headroom = 1.0 - knee;
    float above = v - knee;
    return knee + headroom * (above / (above + headroom));
}

void main()
{
    // Outside the source is transparent, not the clamped edge pixel. Clamping
    // would smear the border outwards along anything scaled or rotated, and it
    // is what the CPU reference does too.
    if (any(lessThan(texCoord, vec2(0.0))) || any(greaterThan(texCoord, vec2(1.0)))) {
        fragColor = vec4(0.0);
        return;
    }

    vec4 sampled = texture(source, texCoord);

    // Keyed and graded un-premultiplied: neither must depend on how faded the
    // clip is, or a grade would change through a dissolve and a matte would
    // move with it.
    if (sampled.a > 0.0001) {
        vec3 straight = sampled.rgb / sampled.a;
        float alpha = sampled.a;
        if (ubuf.keyColour.w > 0.5) {
            float matte = keyMatte(straight);
            if (ubuf.keyFlags.x > 0.5) {
                // The matte itself, kept opaque, so a hole in it shows as grey
                // rather than as a hole.
                fragColor = vec4(vec3(matte) * alpha, alpha) * ubuf.params.x *
                            maskCoverage(framePosition);
                return;
            }
            straight = suppressSpill(straight);
            alpha *= matte;
            if (alpha <= 0.0001) {
                fragColor = vec4(0.0);
                return;
            }
        }
        sampled = vec4(applyGrade(straight) * alpha, alpha);
    }

    // Values are premultiplied, so opacity scales colour and coverage together
    // and a fade stays linear.
    fragColor = sampled * ubuf.params.x * maskCoverage(framePosition);

    // Last of all, and only when presenting: fit what the frame carries into
    // what the screen can show. On straight colour, like the encoder does it --
    // a compressive curve on premultiplied values would make the result depend
    // on how transparent the pixel is.
    if (ubuf.display.x < 1.0 && fragColor.a > 0.0001) {
        vec3 straightOut = fragColor.rgb / fragColor.a;
        straightOut = vec3(rolloff(straightOut.r, ubuf.display.x),
                           rolloff(straightOut.g, ubuf.display.x),
                           rolloff(straightOut.b, ubuf.display.x));
        fragColor.rgb = straightOut * fragColor.a;
    }
}
