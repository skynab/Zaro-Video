#version 440

// A unit quad, transformed into the destination frame.
//
// The CPU reference maps each destination pixel back into the source and
// samples there; the GPU maps the source quad forward and lets the rasteriser
// interpolate. They must agree, which is what the golden-frame tests check.

layout(location = 0) in vec2 position;

layout(location = 0) out vec2 texCoord;

layout(std140, binding = 0) uniform Block {
    mat4 transform;
    vec4 params;  // x: opacity
} ubuf;

void main()
{
    // The quad spans -1..1; texture coordinates span 0..1.
    texCoord = position * 0.5 + 0.5;
    gl_Position = ubuf.transform * vec4(position, 0.0, 1.0);
}
