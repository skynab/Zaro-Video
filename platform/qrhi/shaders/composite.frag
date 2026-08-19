#version 440

layout(location = 0) in vec2 texCoord;

layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D source;

// Tone curves, baked on the CPU into linear-in, linear-out entries. The shader
// never sees a control point or a transfer function: it looks up what
// render::CurveTable already worked out, which is why the two paths cannot
// drift apart. See ADR-012.
layout(binding = 2) uniform sampler2D curveTable;

layout(std140, binding = 0) uniform Block {
    mat4 transform;
    vec4 params;  // x: opacity
    // Primary colour correction, precomputed on the CPU so this shader does
    // not re-derive what a temperature of -20 means. Two implementations of
    // that question are two answers, and preview would disagree with export by
    // an amount too small to notice and too large to accept.
    vec4 balance;  // rgb: white balance gains, w: exposure multiplier
    vec4 grade;    // x: contrast exponent, y: saturation, z: curves active
} ubuf;

const float kMiddleGrey = 0.18;
const vec3 kLumaWeights = vec3(0.2126, 0.7152, 0.0722);

// Must agree with render::gradePixel. It is checked against it.
vec3 applyGrade(vec3 colour)
{
    colour *= ubuf.balance.rgb * ubuf.balance.w;

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

    if (ubuf.grade.z != 0.0) {
        // Must be exactly render::CurveTable::indexFor. Three operations, so
        // there is nothing here to get subtly different.
        vec3 lifted = max(colour, vec3(0.0));
        vec3 index = sqrt(lifted / (vec3(1.0) + lifted));
        colour = vec3(texture(curveTable, vec2(index.r, 0.5)).r,
                      texture(curveTable, vec2(index.g, 0.5)).g,
                      texture(curveTable, vec2(index.b, 0.5)).b);
    }
    return colour;
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

    // Graded un-premultiplied: a correction must not depend on how faded the
    // clip is, or a grade would change through a dissolve.
    if (sampled.a > 0.0001) {
        vec3 straight = sampled.rgb / sampled.a;
        sampled.rgb = applyGrade(straight) * sampled.a;
    }

    // Values are premultiplied, so opacity scales colour and coverage together
    // and a fade stays linear.
    fragColor = sampled * ubuf.params.x;
}
