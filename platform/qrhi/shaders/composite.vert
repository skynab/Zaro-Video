#version 440

// A unit quad, transformed into the destination frame.
//
// The CPU reference maps each destination pixel back into the source and
// samples there; the GPU maps the source quad forward and lets the rasteriser
// interpolate. They must agree, which is what the golden-frame tests check.

layout(location = 0) in vec2 position;

layout(location = 0) out vec2 texCoord;
// Where this fragment lands in the frame, in output pixels from its centre --
// the coordinates a mask is written in. Derived from the clip position rather
// than passed separately, so it cannot disagree with where the quad actually
// goes.
layout(location = 1) out vec2 framePosition;

// One uniform block, shared by every stage of every pipeline, and it has to
// stay that way: qsb lowers the block to a plain `uniform Block ubuf` struct
// for OpenGL, where the vertex and fragment stages link into a single program.
// Two stages declaring `ubuf` with different members are then two conflicting
// declarations of one uniform, and Mesa rejects the program with "uniform
// `ubuf' declared as type `Block' and type `Block'" -- which on the GUI runner
// left every shot in a wipe unpainted. Vulkan, Metal and D3D never noticed,
// because there each stage has its own descriptor.
//
// So the declarations below are identical in composite.vert, composite.frag
// and composite_yuv.frag, down to the order. A field only one of them reads is
// still declared in all three. composite.frag documents what each one carries.
layout(std140, binding = 0) uniform Block {
    mat4 transform;
    vec4 params;  // x: opacity, z: frame width, w: frame height
    vec4 balance;
    vec4 grade;
    vec4 secBalance;
    vec4 secGrade;
    vec4 hueWindow;
    vec4 satWindow;
    vec4 lumaWindow;
    vec4 look;
    vec4 maskBox;
    vec4 maskEdge;
    vec4 keyColour;
    vec4 keyEdge;
    vec4 keyLuma;
    vec4 keyFlags;
    vec4 display;
    vec4 cdlSlope;
    vec4 cdlOffset;
    vec4 cdlPower;
    vec4 vignette;
    vec4 wipeBox;
    vec4 wipeEdge;
    vec4 chroma;
    vec4 coefficients;
    // The source's primaries brought into the working space's, as three rows
    // of a 3x3 in .xyz. Three vec4s rather than a mat3, because std140 pads a
    // mat3's columns to vec4 anyway and this way what is uploaded is what is
    // declared. Only composite_yuv.frag reads them; all three declare them,
    // for the reason in the note above.
    vec4 gamutR;
    vec4 gamutG;
    vec4 gamutB;
} ubuf;

void main()
{
    // The quad spans -1..1; texture coordinates span 0..1.
    texCoord = position * 0.5 + 0.5;
    gl_Position = ubuf.transform * vec4(position, 0.0, 1.0);

    // The projection maps -w/2..w/2 onto -1..1 and flips y, so this undoes it.
    framePosition = vec2(gl_Position.x * ubuf.params.z * 0.5,
                         -gl_Position.y * ubuf.params.w * 0.5);
}
