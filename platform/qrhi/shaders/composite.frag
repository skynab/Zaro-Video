#version 440

layout(location = 0) in vec2 texCoord;

layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D source;

layout(std140, binding = 0) uniform Block {
    mat4 transform;
    vec4 params;  // x: opacity
} ubuf;

void main()
{
    // Outside the source is transparent, not the clamped edge pixel. Clamping
    // would smear the border outwards along anything scaled or rotated, and it
    // is what the CPU reference does too.
    if (any(lessThan(texCoord, vec2(0.0))) || any(greaterThan(texCoord, vec2(1.0)))) {
        fragColor = vec4(0.0);
        return;
    }

    // Values are premultiplied, so opacity scales colour and coverage together
    // and a fade stays linear.
    fragColor = texture(source, texCoord) * ubuf.params.x;
}
