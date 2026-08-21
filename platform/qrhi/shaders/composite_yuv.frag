#version 440

// Colour conversion and compositing in one pass.
//
// The CPU path converts Y'CbCr to the linear working space into an 8MB float
// RGBA buffer and then composites from it. Measured at 1080p that conversion
// ran at 103 fps and the upload of its result cost more than the compositing
// itself. Here the decoder's planes go to the GPU as they are -- 3MB rather
// than 8MB at 1080p -- and the conversion happens per fragment, for free,
// alongside the sampling that has to happen anyway.
//
// This must agree with render::toLinear. It is checked against it.

layout(location = 0) in vec2 texCoord;

layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D planeY;
layout(binding = 2) uniform sampler2D planeCb;   // Cb, or interleaved CbCr
layout(binding = 3) uniform sampler2D planeCr;

layout(std140, binding = 0) uniform Block {
    mat4 transform;
    // opacity, sampleScale, lumaOffset, lumaScale
    vec4 params;
    // chromaScale, midpoint, transferId, semiPlanar
    vec4 chroma;
    // crToR, crToG, cbToG, cbToB
    vec4 coefficients;
} ubuf;

// Transfer curves, matching ColorPipeline.cpp. The CPU samples these into a
// table because std::pow per pixel is ruinous there; a GPU evaluates pow in
// hardware, so the analytic form is both faster and more accurate here.
float curveToLinear(float v, int id)
{
    if (id == 1) {          // Linear
        return v;
    } else if (id == 2) {   // sRGB
        return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
    } else if (id == 3) {   // Gamma 2.2
        return pow(max(v, 0.0), 2.2);
    } else if (id == 4) {   // Gamma 2.8
        return pow(max(v, 0.0), 2.8);
    } else if (id == 5) {   // SMPTE ST 2084 (PQ)
        // Absolute light, normalised so 100 cd/m2 -- SDR diffuse white -- is
        // 1.0. Must match render::curves::kPqReference.
        float m1 = 2610.0 / 16384.0;
        float m2 = 2523.0 / 4096.0 * 128.0;
        float c1 = 3424.0 / 4096.0;
        float c2 = 2413.0 / 4096.0 * 32.0;
        float c3 = 2392.0 / 4096.0 * 32.0;
        float e = pow(max(v, 0.0), 1.0 / m2);
        float den = c2 - c3 * e;
        if (den <= 0.0) {
            return 0.0;
        }
        return pow(max(e - c1, 0.0) / den, 1.0 / m1) * (10000.0 / 100.0);
    } else if (id == 6) {   // Hybrid log gamma
        float a = 0.17883277;
        float b = 0.28466892;
        float c = 0.55991073;
        float x = clamp(v, 0.0, 1.0);
        return x <= 0.5 ? (x * x) / 3.0 : (exp((x - c) / a) + b) / 12.0;
    } else if (id == 7) {   // Sony S-Log3
        if (v >= 171.2102946929 / 1023.0) {
            return pow(10.0, ((v * 1023.0) - 420.0) / 261.5) * 0.19 - 0.01;
        }
        return ((v * 1023.0) - 95.0) * 0.01125000 / (171.2102946929 - 95.0);
    } else if (id == 8) {   // Panasonic V-Log
        if (v < 0.181) {
            return (v - 0.125) / 5.6;
        }
        return pow(10.0, (v - 0.598206) / 0.241514) - 0.00873;
    } else if (id == 9) {   // Arri LogC3, EI 800
        if (v > 5.367655 * 0.010591 + 0.092809) {
            return (pow(10.0, (v - 0.385537) / 0.247190) - 0.052272) / 5.555556;
        }
        return (v - 0.092809) / 5.367655;
    }
    // BT.709 / SMPTE 170M inverse OETF.
    return v < 0.081 ? v / 4.5 : pow((v + 0.099) / 1.099, 1.0 / 0.45);
}

void main()
{
    // Outside the source is transparent, not the clamped edge pixel.
    if (any(lessThan(texCoord, vec2(0.0))) || any(greaterThan(texCoord, vec2(1.0)))) {
        fragColor = vec4(0.0);
        return;
    }

    float scale = ubuf.params.y;
    float luma = texture(planeY, texCoord).r * scale;

    float cb;
    float cr;
    if (ubuf.chroma.w > 0.5) {
        // NV12 and P010 interleave Cb and Cr in one plane.
        vec2 pair = texture(planeCb, texCoord).rg * scale;
        cb = pair.x;
        cr = pair.y;
    } else {
        cb = texture(planeCb, texCoord).r * scale;
        cr = texture(planeCr, texCoord).r * scale;
    }

    float yy = (luma - ubuf.params.z) * ubuf.params.w;
    // Chroma is centred on the midpoint of its range, not on zero.
    float cbCentred = (cb - ubuf.chroma.y) * ubuf.chroma.x;
    float crCentred = (cr - ubuf.chroma.y) * ubuf.chroma.x;

    vec3 encoded = vec3(yy + ubuf.coefficients.x * crCentred,
                        yy - ubuf.coefficients.y * crCentred - ubuf.coefficients.z * cbCentred,
                        yy + ubuf.coefficients.w * cbCentred);
    encoded = clamp(encoded, 0.0, 1.0);

    int transferId = int(ubuf.chroma.z + 0.5);
    vec3 linear = vec3(curveToLinear(encoded.r, transferId),
                       curveToLinear(encoded.g, transferId),
                       curveToLinear(encoded.b, transferId));

    // Source video has no alpha, so coverage is 1 and premultiplying is the
    // opacity multiply alone.
    fragColor = vec4(linear, 1.0) * ubuf.params.x;
}
