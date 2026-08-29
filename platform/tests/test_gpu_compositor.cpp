#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/media/VideoFrame.h"
#include "zaro/core/model/Mask.h"
#include "zaro/core/render/ColorPipeline.h"
#include "zaro/core/render/Compositing.h"
#include "zaro/core/render/RenderGraph.h"
#include "zaro/core/render/ToneMap.h"
#include "zaro/core/render/TransitionShape.h"
#include "zaro/platform/qrhi/GpuCompositor.h"
#include "zaro/platform/qrhi/GpuRenderGraph.h"

#include "ModelFixtures.h"
#include "StatusChecks.h"

using namespace zaro;
using Catch::Approx;
using model::BlendMode;
using model::Transform;
using render::Rgba;
using render::RgbaImage;

namespace {

Rgba premultiplied(float r, float g, float b, float a) {
    return Rgba{r * a, g * a, b * a, a};
}

RgbaImage filled(std::int32_t width, std::int32_t height, const Rgba& colour) {
    RgbaImage image{width, height};
    image.fill(colour);
    return image;
}

/// A pattern with structure in it, so a transform that is subtly wrong shows up
/// as a mismatch rather than as the same flat colour in the wrong place.
RgbaImage gradient(std::int32_t width, std::int32_t height) {
    RgbaImage image{width, height};
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(width - 1);
            const float v = static_cast<float>(y) / static_cast<float>(height - 1);
            image.at(x, y) = premultiplied(u, v, 1.0F - u, 1.0F);
        }
    }
    return image;
}

struct Difference {
    float worst{0.0F};
    float mean{0.0F};
    std::int32_t worstX{0};
    std::int32_t worstY{0};
    /// Pixels differing by more than a quantisation step at 8 bits.
    std::int64_t differing{0};
};

/// Compare two frames, ignoring a border of `margin` pixels.
///
/// The two paths disagree along the *outline of the transformed clip*, not just
/// at the image border. The CPU reference maps each destination pixel back into
/// the source and treats anything outside as transparent, so it produces a
/// partially covered edge pixel; the GPU rasterises a quad, so coverage is
/// decided by the rasteriser. The difference is real, is confined to a
/// one-pixel outline, and is under half a pixel of geometry.
///
/// So the assertion is not "every pixel matches" -- that would be false and
/// papering over it with a large tolerance would hide a genuine misalignment.
/// It is "the interior matches closely, and the pixels that differ form a thin
/// outline rather than an area". `differing` is what makes the second half
/// checkable.
Difference compare(const RgbaImage& a, const RgbaImage& b, std::int32_t margin) {
    Difference result;
    double total = 0.0;
    std::int64_t counted = 0;

    for (std::int32_t y = margin; y < a.height() - margin; ++y) {
        for (std::int32_t x = margin; x < a.width() - margin; ++x) {
            const Rgba& left = a.at(x, y);
            const Rgba& right = b.at(x, y);
            const float delta = std::max({std::abs(left.r - right.r), std::abs(left.g - right.g),
                                          std::abs(left.b - right.b), std::abs(left.a - right.a)});
            total += static_cast<double>(delta);
            ++counted;
            if (delta > 1.0F / 255.0F) {
                ++result.differing;
            }
            if (delta > result.worst) {
                result.worst = delta;
                result.worstX = x;
                result.worstY = y;
            }
        }
    }
    result.mean = counted > 0 ? static_cast<float>(total / static_cast<double>(counted)) : 0.0F;
    return result;
}

std::unique_ptr<platform::qrhi::GpuCompositor> gpu() {
    auto created = platform::qrhi::GpuCompositor::create();
    if (!created) {
        return nullptr;
    }
    return std::move(*created);
}

/// Run the same composite through both paths.
struct Pair {
    RgbaImage cpu;
    RgbaImage gpu;
};

Pair renderBoth(platform::qrhi::GpuCompositor& compositor, const RgbaImage& source,
                const Transform& transform, BlendMode blend, std::int32_t width,
                std::int32_t height, const Rgba& background = Rgba{}) {
    Pair pair;
    pair.cpu = RgbaImage{width, height};
    if (background.a > 0.0F || background.r > 0.0F) {
        pair.cpu.fill(background);
    }
    render::drawTransformed(source, pair.cpu, transform, blend);

    ZARO_REQUIRE_OK(compositor.beginFrame(width, height));
    if (background.a > 0.0F || background.r > 0.0F) {
        RgbaImage backdrop = filled(width, height, background);
        ZARO_REQUIRE_OK(compositor.draw(backdrop, Transform{}, BlendMode::Normal));
    }
    ZARO_REQUIRE_OK(compositor.draw(source, transform, blend));
    ZARO_REQUIRE_OK(compositor.endFrame(pair.gpu));
    return pair;
}

}  // namespace

TEST_CASE("A GPU backend is available", "[gpu]") {
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());
    INFO("backend: " << compositor->backendName());
    CHECK_FALSE(compositor->backendName().empty());
}

TEST_CASE("An empty GPU frame is transparent", "[gpu]") {
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());
    ZARO_REQUIRE_OK(compositor->beginFrame(16, 16));
    RgbaImage out;
    ZARO_REQUIRE_OK(compositor->endFrame(out));
    CHECK(out.width() == 16);
    CHECK(out.at(8, 8).a == Approx(0.0F).margin(1e-6));
}

TEST_CASE("GPU and CPU agree on an untransformed draw", "[gpu][golden]") {
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());
    const RgbaImage source = gradient(64, 64);
    const Pair pair = renderBoth(*compositor, source, Transform{}, BlendMode::Normal, 64, 64);

    const Difference difference = compare(pair.cpu, pair.gpu, 1);
    INFO("worst " << difference.worst << " at " << difference.worstX << "," << difference.worstY
                  << ", mean " << difference.mean);
    // A 1:1 draw should be near exact: both paths are sampling texel centres.
    CHECK(difference.worst < 0.01F);
    CHECK(difference.mean < 0.001F);
}

TEST_CASE("GPU and CPU agree on scale, position and opacity", "[gpu][golden]") {
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());
    const RgbaImage source = gradient(32, 32);

    struct Case {
        const char* name;
        Transform transform;
    };
    std::vector<Case> cases;
    {
        Transform scaled;
        scaled.scaleX = 2.0;
        scaled.scaleY = 2.0;
        cases.push_back({"scale 2x", scaled});

        Transform shrunk;
        shrunk.scaleX = 0.5;
        shrunk.scaleY = 0.5;
        cases.push_back({"scale 0.5x", shrunk});

        Transform moved;
        moved.positionX = 12.0;
        moved.positionY = -8.0;
        cases.push_back({"offset", moved});

        Transform faded;
        faded.opacity = 0.35;
        cases.push_back({"opacity", faded});

        Transform combined;
        combined.scaleX = 1.5;
        combined.scaleY = 1.5;
        combined.positionX = 6.0;
        combined.opacity = 0.6;
        cases.push_back({"combined", combined});
    }

    for (const Case& testCase : cases) {
        const Pair pair =
            renderBoth(*compositor, source, testCase.transform, BlendMode::Normal, 96, 96);
        const Difference difference = compare(pair.cpu, pair.gpu, 2);
        INFO(testCase.name << ": worst " << difference.worst << " at " << difference.worstX << ","
                           << difference.worstY << ", mean " << difference.mean << ", "
                           << difference.differing << " pixels differ");

        // The interior agrees: the average difference across the whole frame is
        // far below a quantisation step at 8 bits.
        CHECK(difference.mean < 0.02F);

        // And what differs is an outline, not an area. The drawn quad is at
        // most 96x96 here, so its perimeter is under 400 pixels; anything much
        // beyond that would mean the two paths disagree about the picture
        // rather than about its edge.
        CHECK(difference.differing < 800);
    }
}

TEST_CASE("GPU and CPU agree on rotation, including its direction", "[gpu][golden]") {
    // Direction matters: a matrix with the sign flipped produces a picture that
    // looks perfectly plausible until compared against the reference.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());
    const RgbaImage source = gradient(32, 32);

    for (const double degrees : {15.0, 45.0, 90.0, -30.0, 180.0}) {
        Transform transform;
        transform.rotationDegrees = degrees;
        const Pair pair = renderBoth(*compositor, source, transform, BlendMode::Normal, 96, 96);
        const Difference difference = compare(pair.cpu, pair.gpu, 2);
        INFO(degrees << " degrees: worst " << difference.worst << " at " << difference.worstX << ","
                     << difference.worstY << ", mean " << difference.mean << ", "
                     << difference.differing << " pixels differ");
        CHECK(difference.mean < 0.02F);
        // A rotated quad has a longer, staircased outline than an axis-aligned
        // one, so the allowance is larger -- but still an outline.
        CHECK(difference.differing < 1200);
    }
}

TEST_CASE("GPU and CPU agree on blend modes", "[gpu][golden]") {
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());
    const RgbaImage source = filled(32, 32, premultiplied(0.5F, 0.5F, 0.5F, 1.0F));
    const Rgba background = premultiplied(0.5F, 0.25F, 0.75F, 1.0F);

    for (const BlendMode blend :
         {BlendMode::Normal, BlendMode::Add, BlendMode::Multiply, BlendMode::Screen}) {
        const Pair pair = renderBoth(*compositor, source, Transform{}, blend, 32, 32, background);
        const Difference difference = compare(pair.cpu, pair.gpu, 1);
        INFO(model::toString(blend)
             << ": worst " << difference.worst << ", mean " << difference.mean);
        CHECK(difference.worst < 0.01F);
    }
}

TEST_CASE("A half-transparent GPU draw composites over what is beneath", "[gpu][golden]") {
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());
    const RgbaImage source = filled(16, 16, premultiplied(1.0F, 0.0F, 0.0F, 1.0F));
    Transform transform;
    transform.opacity = 0.5;

    const Pair pair = renderBoth(*compositor, source, transform, BlendMode::Normal, 16, 16,
                                 premultiplied(0.0F, 0.0F, 1.0F, 1.0F));
    CHECK(pair.gpu.at(8, 8).r == Approx(0.5F).margin(0.01));
    CHECK(pair.gpu.at(8, 8).b == Approx(0.5F).margin(0.01));
    CHECK(compare(pair.cpu, pair.gpu, 1).worst < 0.01F);
}

TEST_CASE("Compositing throughput, CPU against GPU", "[.benchmark][gpu]") {
    // Hidden by default; run with `zaro_media_tests [.benchmark]`.
    //
    // The number that decides whether realtime HD is reachable. Phase 3b
    // measured the CPU compositor at about 9 fps on a 1080p timeline against a
    // decoder managing 1840, so essentially all the frame time is here.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    constexpr std::int32_t kWidth = 1920;
    constexpr std::int32_t kHeight = 1080;
    constexpr int kFrames = 60;

    const RgbaImage source = gradient(kWidth, kHeight);
    Transform transform;
    transform.scaleX = 1.05;
    transform.scaleY = 1.05;
    transform.opacity = 0.9;

    RgbaImage cpuOut{kWidth, kHeight};
    const auto cpuStart = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
        cpuOut.clear();
        render::drawTransformed(source, cpuOut, transform, BlendMode::Normal);
    }
    const double cpuSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - cpuStart).count();

    RgbaImage gpuOut;
    const auto gpuStart = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
        ZARO_REQUIRE_OK(compositor->beginFrame(kWidth, kHeight));
        ZARO_REQUIRE_OK(compositor->draw(source, transform, BlendMode::Normal));
        ZARO_REQUIRE_OK(compositor->endFrame(gpuOut));
    }
    const double gpuSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - gpuStart).count();

    const double cpuFps = kFrames / cpuSeconds;
    const double gpuFps = kFrames / gpuSeconds;

    WARN("1080p compositing: CPU " << cpuFps << " fps, GPU " << gpuFps << " fps ("
                                   << (gpuFps / cpuFps) << "x). The GPU figure includes an 8MB "
                                   << "upload and an 8MB readback every frame -- the round trip "
                                   << "this design exists to remove, and currently more "
                                   << "expensive than the compositing it replaces.");
    // Deliberately no assertion about which is faster. The honest result today
    // is that the round trip costs more than it saves, exactly as it did for
    // hardware decode in ADR-003, and a test that asserted otherwise would be
    // asserting a wish. What must hold is that both paths produce the same
    // picture, which the golden-frame tests above check.
    CHECK(cpuFps > 0.0);
    CHECK(gpuFps > 0.0);
}

TEST_CASE("Where the frame time actually goes at 1080p", "[.benchmark][gpu]") {
    // Phase 3b measured the whole pipeline at about 9 fps on 1080p while decode
    // alone managed 1840. The benchmark above shows compositing manages 63, so
    // neither of those is the bottleneck. This finds the piece that is.
    constexpr std::int32_t kWidth = 1920;
    constexpr std::int32_t kHeight = 1080;
    constexpr int kFrames = 30;

    media::VideoFrame yuv =
        media::VideoFrame::allocate(kWidth, kHeight, media::PixelFormat::YUV420P);
    yuv.setColor(media::ColorInfo{media::ColorPrimaries::BT709, media::TransferFunction::BT709,
                                  media::ColorMatrix::BT709, media::ColorRange::Limited});
    for (std::size_t plane = 0; plane < yuv.planeCount(); ++plane) {
        const auto index = static_cast<std::int32_t>(plane);
        const auto rows =
            static_cast<std::size_t>(media::planeHeight(yuv.format(), kHeight, index));
        const auto bytes = static_cast<std::size_t>(media::rowBytes(yuv.format(), kWidth, index));
        for (std::size_t row = 0; row < rows; ++row) {
            std::fill_n(yuv.plane(plane) + row * static_cast<std::size_t>(yuv.stride(plane)), bytes,
                        static_cast<std::uint8_t>(plane == 0 ? 120 : 128));
        }
    }

    RgbaImage linear;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
        ZARO_REQUIRE_OK(render::toLinear(yuv, linear));
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    WARN("1080p YUV to linear working space: " << (kFrames / seconds) << " fps. This is the "
                                               << "conversion that runs before compositing, and "
                                               << "it is the piece to move into the shader.");
    CHECK(seconds > 0.0);
}

namespace {

/// A Y'CbCr frame with structure in every plane, so a mistake in the matrix,
/// the range scaling or the chroma indexing shows up as a colour shift rather
/// than as the same flat value.
media::VideoFrame yuvPattern(std::int32_t width, std::int32_t height, media::PixelFormat format,
                             media::ColorRange range, media::ColorMatrix matrix) {
    media::VideoFrame frame = media::VideoFrame::allocate(width, height, format);
    frame.setColor(media::ColorInfo{media::ColorPrimaries::BT709, media::TransferFunction::BT709,
                                    matrix, range});
    const media::PixelFormatInfo& info = media::info(format);
    const bool deep = info.bitsPerComponent > 8;
    const int shift = format == media::PixelFormat::P010 ? 6 : 0;

    for (std::size_t plane = 0; plane < frame.planeCount(); ++plane) {
        const auto index = static_cast<std::int32_t>(plane);
        const std::int32_t rows = media::planeHeight(format, height, index);
        const std::int32_t bytes = media::rowBytes(format, width, index);
        const std::int32_t samples = deep ? bytes / 2 : bytes;

        for (std::int32_t row = 0; row < rows; ++row) {
            std::uint8_t* line =
                frame.plane(plane) +
                static_cast<std::size_t>(row) * static_cast<std::size_t>(frame.stride(plane));
            for (std::int32_t i = 0; i < samples; ++i) {
                // Luma sweeps across, chroma varies down, so every plane
                // contributes something distinguishable.
                const int base = plane == 0 ? 20 + ((i * 200) / std::max(1, samples - 1))
                                            : 40 + ((row * 170) / std::max(1, rows - 1)) +
                                                  (static_cast<int>(plane) * 13) + (i % 7);
                const int value = std::clamp(base, 0, 255);
                if (deep) {
                    const auto wide = static_cast<std::uint16_t>((value << 2) << shift);
                    std::memcpy(line + static_cast<std::size_t>(i) * 2, &wide, sizeof(wide));
                } else {
                    line[i] = static_cast<std::uint8_t>(value);
                }
            }
        }
    }
    return frame;
}

}  // namespace

TEST_CASE("The GPU converts Y'CbCr exactly as the CPU does", "[gpu][golden][yuv]") {
    // The whole point of Phase 3d: the colour conversion moves into the shader,
    // so it has to produce the same picture the reference does.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    struct Case {
        const char* name;
        media::PixelFormat format;
        media::ColorRange range;
        media::ColorMatrix matrix;
    };
    const Case cases[] = {
        {"8-bit 4:2:0 limited 709", media::PixelFormat::YUV420P, media::ColorRange::Limited,
         media::ColorMatrix::BT709},
        {"8-bit 4:2:0 full 709", media::PixelFormat::YUV420P, media::ColorRange::Full,
         media::ColorMatrix::BT709},
        {"8-bit 4:2:0 limited 601", media::PixelFormat::YUV420P, media::ColorRange::Limited,
         media::ColorMatrix::BT601},
        {"8-bit 4:2:2 limited 709", media::PixelFormat::YUV422P, media::ColorRange::Limited,
         media::ColorMatrix::BT709},
        {"8-bit 4:4:4 limited 709", media::PixelFormat::YUV444P, media::ColorRange::Limited,
         media::ColorMatrix::BT709},
        {"10-bit 4:2:2 limited 709", media::PixelFormat::YUV422P10, media::ColorRange::Limited,
         media::ColorMatrix::BT709},
        {"NV12 limited 709", media::PixelFormat::NV12, media::ColorRange::Limited,
         media::ColorMatrix::BT709},
    };

    for (const Case& testCase : cases) {
        const media::VideoFrame frame =
            yuvPattern(64, 64, testCase.format, testCase.range, testCase.matrix);

        // Reference: convert on the CPU, then composite.
        RgbaImage converted;
        ZARO_REQUIRE_OK(render::toLinear(frame, converted));
        RgbaImage cpuOut{64, 64};
        render::drawTransformed(converted, cpuOut, Transform{}, BlendMode::Normal);

        // Under test: hand the planes to the GPU and let the shader do both.
        ZARO_REQUIRE_OK(compositor->beginFrame(64, 64));
        const auto drawn =
            compositor->drawSource(frame, Transform{}, render::GradeConstants{}, BlendMode::Normal);
        ZARO_REQUIRE_OK(drawn);
        RgbaImage gpuOut;
        ZARO_REQUIRE_OK(compositor->endFrame(gpuOut));

        const Difference difference = compare(cpuOut, gpuOut, 1);
        INFO(testCase.name << ": worst " << difference.worst << " at " << difference.worstX << ","
                           << difference.worstY << ", mean " << difference.mean << ", "
                           << difference.differing << " pixels differ");
        // The CPU samples its transfer curve from a 4096-entry table and the
        // GPU evaluates it analytically, so they differ by the table's
        // interpolation error -- far below an 8-bit step.
        CHECK(difference.worst < 0.01F);
        CHECK(difference.mean < 0.002F);
    }
}

TEST_CASE("The GPU YUV path honours transforms", "[gpu][golden][yuv]") {
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    Transform transform;
    transform.scaleX = 1.75;
    transform.scaleY = 1.75;
    transform.positionX = 5.0;
    transform.rotationDegrees = 20.0;
    transform.opacity = 0.7;

    const auto run = [&](media::PixelFormat format) {
        const media::VideoFrame frame =
            yuvPattern(32, 32, format, media::ColorRange::Limited, media::ColorMatrix::BT709);
        RgbaImage converted;
        ZARO_REQUIRE_OK(render::toLinear(frame, converted));
        RgbaImage cpuOut{96, 96};
        render::drawTransformed(converted, cpuOut, transform, BlendMode::Normal);

        ZARO_REQUIRE_OK(compositor->beginFrame(96, 96));
        ZARO_REQUIRE_OK(
            compositor->drawSource(frame, transform, render::GradeConstants{}, BlendMode::Normal));
        RgbaImage gpuOut;
        ZARO_REQUIRE_OK(compositor->endFrame(gpuOut));
        return compare(cpuOut, gpuOut, 2);
    };

    SECTION("4:4:4, where the two paths are directly comparable") {
        // No chroma subsampling, so the only thing under test is the geometry
        // and the conversion. This is the strict case.
        const Difference difference = run(media::PixelFormat::YUV444P);
        INFO("worst " << difference.worst << ", mean " << difference.mean << ", "
                      << difference.differing << " pixels differ");
        CHECK(difference.mean < 0.01F);
        CHECK(difference.differing < 1200);
    }

    SECTION("4:2:0, where chroma is resampled in a different order") {
        // A genuine difference, not an edge artefact, and worth stating plainly:
        //
        //   CPU: upsample chroma (nearest) -> convert to RGB -> scale bilinearly
        //   GPU: sample planes at the destination scale -> convert per fragment
        //
        // So when a clip is scaled up, the CPU smooths chroma along with
        // everything else after conversion, while the GPU keeps it blocky at the
        // destination scale. Neither is wrong; they are different orderings, and
        // the GPU's is what doing conversion in the sampler buys.
        //
        // The unsubsampled case above pins the geometry and the conversion, so
        // what is left here is exactly the chroma ordering. It is bounded and
        // small on average; on this fixture, whose chroma deliberately changes
        // every few samples, it is far larger than real footage would produce.
        const Difference difference = run(media::PixelFormat::YUV420P);
        INFO("worst " << difference.worst << ", mean " << difference.mean << ", "
                      << difference.differing << " pixels differ");
        CHECK(difference.mean < 0.02F);
        CHECK(difference.worst < 0.5F);
    }
}

TEST_CASE("The GPU YUV path refuses what it cannot handle", "[gpu][yuv]") {
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());
    ZARO_REQUIRE_OK(compositor->beginFrame(16, 16));

    // A curve with no formula. PQ and HLG used to be here; they have formulas
    // now, and what is left to refuse is a tag nothing knows how to invert --
    // where guessing produces a picture that is wrong in a way nobody can see
    // is the tag rather than the footage.
    media::VideoFrame untagged = yuvPattern(16, 16, media::PixelFormat::YUV420P,
                                            media::ColorRange::Limited, media::ColorMatrix::BT709);
    media::ColorInfo unknown = untagged.color();
    unknown.transfer = media::TransferFunction::Unknown;
    untagged.setColor(unknown);

    const auto status =
        compositor->drawSource(untagged, Transform{}, render::GradeConstants{}, BlendMode::Normal);
    REQUIRE_FALSE(status.ok());
    CHECK(status.error().code() == ErrorCode::Unsupported);

    // And the HDR curves go through.
    media::VideoFrame hdr = yuvPattern(16, 16, media::PixelFormat::YUV420P,
                                       media::ColorRange::Limited, media::ColorMatrix::BT709);
    media::ColorInfo pq = hdr.color();
    pq.transfer = media::TransferFunction::PQ;
    hdr.setColor(pq);
    ZARO_CHECK_OK(
        compositor->drawSource(hdr, Transform{}, render::GradeConstants{}, BlendMode::Normal));

    RgbaImage out;
    ZARO_REQUIRE_OK(compositor->endFrame(out));
}

TEST_CASE("The YUV path against the CPU pipeline it replaces", "[.benchmark][gpu][yuv]") {
    // Phase 3c measured the CPU at 103 fps for colour conversion and 63 fps for
    // compositing at 1080p, and the GPU at 54 fps once an 8MB upload and an 8MB
    // readback were paid for. This measures what Phase 3d was for: uploading
    // the decoder's planes instead, and converting on the way through.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    constexpr std::int32_t kWidth = 1920;
    constexpr std::int32_t kHeight = 1080;
    constexpr int kFrames = 60;

    const media::VideoFrame frame =
        yuvPattern(kWidth, kHeight, media::PixelFormat::YUV420P, media::ColorRange::Limited,
                   media::ColorMatrix::BT709);
    Transform transform;
    transform.scaleX = 1.05;
    transform.scaleY = 1.05;
    transform.opacity = 0.9;

    // The CPU pipeline: convert, then composite.
    RgbaImage converted;
    RgbaImage cpuOut{kWidth, kHeight};
    const auto cpuStart = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
        ZARO_REQUIRE_OK(render::toLinear(frame, converted));
        cpuOut.clear();
        render::drawTransformed(converted, cpuOut, transform, BlendMode::Normal);
    }
    const double cpuSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - cpuStart).count();

    // The GPU pipeline as a preview would run it: planes up, nothing back.
    const auto previewStart = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
        ZARO_REQUIRE_OK(compositor->beginFrame(kWidth, kHeight));
        ZARO_REQUIRE_OK(
            compositor->drawSource(frame, transform, render::GradeConstants{}, BlendMode::Normal));
        ZARO_REQUIRE_OK(compositor->endFrameOnGpu());
    }
    const double previewSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - previewStart).count();

    // And as an export would, which still has to read the result back.
    RgbaImage gpuOut;
    const auto readbackStart = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
        ZARO_REQUIRE_OK(compositor->beginFrame(kWidth, kHeight));
        ZARO_REQUIRE_OK(
            compositor->drawSource(frame, transform, render::GradeConstants{}, BlendMode::Normal));
        ZARO_REQUIRE_OK(compositor->endFrame(gpuOut));
    }
    const double readbackSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - readbackStart).count();

    WARN("1080p, convert and composite:"
         << "\n    CPU pipeline            " << (kFrames / cpuSeconds) << " fps"
         << "\n    GPU, result stays on it " << (kFrames / previewSeconds) << " fps"
         << "\n    GPU, read back to CPU   " << (kFrames / readbackSeconds) << " fps");
    CHECK(cpuSeconds > 0.0);
    CHECK(previewSeconds > 0.0);
}

TEST_CASE("Presenting preserves orientation and letterboxes", "[gpu][golden]") {
    // The bug this exists for: the preview rendered vertically flipped, and a
    // check that counted lit pixels reported 96.8% either way. Orientation and
    // letterbox geometry are invisible to any test that only asks whether
    // something was drawn.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    // Asymmetric on purpose: white across the top third, black below.
    RgbaImage source{32, 30};
    for (std::int32_t y = 0; y < 30; ++y) {
        for (std::int32_t x = 0; x < 32; ++x) {
            source.at(x, y) = y < 10 ? premultiplied(1.0F, 1.0F, 1.0F, 1.0F)
                                     : premultiplied(0.0F, 0.0F, 0.0F, 1.0F);
        }
    }

    ZARO_REQUIRE_OK(compositor->beginFrame(32, 30));
    ZARO_REQUIRE_OK(compositor->draw(source, Transform{}, BlendMode::Normal));
    ZARO_REQUIRE_OK(compositor->endFrameOnGpu());

    SECTION("the top of the picture stays at the top") {
        RgbaImage presented;
        // Same aspect ratio, so there are no bars to complicate it.
        ZARO_REQUIRE_OK(compositor->presentToImage(64, 60, presented));
        CHECK(presented.at(32, 5).r > 0.5F);   // near the top: white
        CHECK(presented.at(32, 50).r < 0.5F);  // near the bottom: black
    }

    SECTION("a wider target gets bars at the sides, not the top") {
        RgbaImage presented;
        ZARO_REQUIRE_OK(compositor->presentToImage(160, 60, presented));
        // The frame is 32x30, so in a 160x60 target it occupies the middle
        // 64 pixels horizontally and the full height.
        CHECK(presented.at(4, 30).a > 0.5F);  // bar: opaque black backdrop
        CHECK(presented.at(4, 30).r < 0.1F);
        CHECK(presented.at(80, 5).r > 0.5F);    // picture, still white on top
        CHECK(presented.at(156, 30).r < 0.1F);  // bar on the other side
    }

    SECTION("a taller target gets bars above and below") {
        RgbaImage presented;
        ZARO_REQUIRE_OK(compositor->presentToImage(64, 200, presented));
        CHECK(presented.at(32, 4).r < 0.1F);    // bar at the top
        CHECK(presented.at(32, 196).r < 0.1F);  // bar at the bottom
        // Picture occupies the middle 60 rows: 70..130. Its own top third is
        // white, so sample just inside that.
        CHECK(presented.at(32, 78).r > 0.5F);
    }
}

namespace {

/// A SourceFrameProvider yielding flat Y'CbCr frames, so the GPU and CPU paths
/// can be run over the same sequence and compared.
class SolidSourceProvider final : public zaro::render::SourceFrameProvider {
public:
    void define(zaro::model::MediaRefId media, std::uint8_t luma) { lumas_[media.value()] = luma; }

    zaro::Result<const zaro::media::VideoFrame*> sourceFrameFor(
        zaro::model::MediaRefId media, const zaro::time::RationalTime& sourceTime) override {
        requests.push_back(sourceTime);
        const auto found = lumas_.find(media.value());
        if (found == lumas_.end()) {
            return zaro::Error{zaro::ErrorCode::NotFound, "no such media"};
        }
        current_ = zaro::media::VideoFrame::allocate(16, 16, zaro::media::PixelFormat::YUV420P);
        current_.setColor(zaro::media::ColorInfo{
            zaro::media::ColorPrimaries::BT709, zaro::media::TransferFunction::BT709,
            zaro::media::ColorMatrix::BT709, zaro::media::ColorRange::Limited});
        for (std::size_t plane = 0; plane < current_.planeCount(); ++plane) {
            const auto index = static_cast<std::int32_t>(plane);
            const auto rows =
                static_cast<std::size_t>(zaro::media::planeHeight(current_.format(), 16, index));
            const auto bytes =
                static_cast<std::size_t>(zaro::media::rowBytes(current_.format(), 16, index));
            for (std::size_t row = 0; row < rows; ++row) {
                std::fill_n(
                    current_.plane(plane) + row * static_cast<std::size_t>(current_.stride(plane)),
                    bytes, plane == 0 ? found->second : std::uint8_t{128});
            }
        }
        return &current_;
    }

    std::vector<zaro::time::RationalTime> requests;

private:
    std::map<std::uint64_t, std::uint8_t> lumas_;
    zaro::media::VideoFrame current_;
};

}  // namespace

TEST_CASE("The GPU and CPU render graphs agree across a dissolve", "[gpu][golden][transition]") {
    // The two traversals are written separately, so nothing but a test keeps
    // them in step -- and a preview that disagrees with the export is the worst
    // kind of disagreement, because it is only discovered after delivery.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    zaro::testing::Fixture f;
    f.sequence().setSize(16, 16);
    REQUIRE(
        f.run(zaro::edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500, f.longMedia))));
    REQUIRE(
        f.run(zaro::edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 20, f.shortMedia))));
    REQUIRE(f.run(zaro::edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(50), f.at(10))));

    SolidSourceProvider gpuProvider;
    gpuProvider.define(f.longMedia, 60);
    gpuProvider.define(f.shortMedia, 200);
    zaro::platform::qrhi::GpuRenderGraph gpuGraph{*compositor, gpuProvider};

    // The CPU graph takes images already in the working space, so feed it the
    // same frames converted the same way.
    class Adapter final : public zaro::render::FrameSource {
    public:
        explicit Adapter(SolidSourceProvider& provider) : provider_{&provider} {}
        zaro::Result<const RgbaImage*> imageFor(zaro::model::MediaRefId media,
                                                const zaro::time::RationalTime& at) override {
            auto frame = provider_->sourceFrameFor(media, at);
            if (!frame) {
                return frame.error();
            }
            if (const auto status = zaro::render::toLinear(**frame, current_); !status) {
                return status.error();
            }
            return &current_;
        }

    private:
        SolidSourceProvider* provider_;
        RgbaImage current_;
    };
    SolidSourceProvider cpuProvider;
    cpuProvider.define(f.longMedia, 60);
    cpuProvider.define(f.shortMedia, 200);
    Adapter adapter{cpuProvider};
    zaro::render::RenderGraph cpuGraph{adapter};

    for (const std::int64_t frame : {40, 45, 48, 50, 52, 54, 60}) {
        const auto at = f.at(frame);

        RgbaImage onGpu;
        ZARO_REQUIRE_OK(gpuGraph.compositeInto(f.sequence(), at, onGpu));
        RgbaImage onCpu;
        ZARO_REQUIRE_OK(cpuGraph.compositeInto(f.sequence(), at, onCpu));

        const Difference difference = compare(onCpu, onGpu, 1);
        INFO("frame " << frame << ": worst " << difference.worst << ", mean " << difference.mean);
        CHECK(difference.mean < 0.02F);
        CHECK(difference.worst < 0.05F);
    }
}

TEST_CASE("The GPU grade agrees with the CPU reference", "[gpu][golden][grade]") {
    // The two implementations of the grade are separate code, and a correction
    // honoured on export but not in preview -- or applied slightly differently
    // -- is invisible until someone compares a delivered file against what they
    // approved. So the shader is checked against render::gradePixel directly.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    struct Case {
        const char* name;
        model::ColorCorrection correction;
    };
    const auto make = [](double temperature, double tint, double exposure, double contrast,
                         double saturation) {
        model::ColorCorrection out;
        out.temperature = temperature;
        out.tint = tint;
        out.exposure = exposure;
        out.contrast = contrast;
        out.saturation = saturation;
        return out;
    };
    const Case cases[] = {
        {"neutral", make(0, 0, 0, 0, 100)},
        {"warm", make(60, 0, 0, 0, 100)},
        {"cool and green", make(-45, -30, 0, 0, 100)},
        {"one stop up", make(0, 0, 1.0, 0, 100)},
        {"two stops down", make(0, 0, -2.0, 0, 100)},
        {"contrast up", make(0, 0, 0, 60, 100)},
        {"contrast down", make(0, 0, 0, -60, 100)},
        {"monochrome", make(0, 0, 0, 0, 0)},
        {"oversaturated", make(0, 0, 0, 0, 175)},
        {"everything at once", make(35, -20, 0.75, 40, 130)},
    };

    // A spread of colours and brightnesses, including values above 1 -- the
    // working space is scene-linear and a highlight is allowed to exceed white.
    RgbaImage source{32, 32};
    for (std::int32_t y = 0; y < 32; ++y) {
        Rgba* row = source.row(y);
        for (std::int32_t x = 0; x < 32; ++x) {
            const float u = static_cast<float>(x) / 31.0F;
            const float v = static_cast<float>(y) / 31.0F;
            row[x] = Rgba{u * 1.4F, v, (1.0F - u) * 0.8F, 1.0F};
        }
    }

    for (const Case& testCase : cases) {
        const auto grade = render::gradeConstantsFor(testCase.correction);

        RgbaImage cpuOut{32, 32};
        render::drawTransformed(source, cpuOut, Transform{}, BlendMode::Normal,
                                {.grade = grade.isIdentity() ? nullptr : &grade});

        ZARO_REQUIRE_OK(compositor->beginFrame(32, 32));
        ZARO_REQUIRE_OK(compositor->draw(source, Transform{}, BlendMode::Normal, grade));
        RgbaImage gpuOut;
        ZARO_REQUIRE_OK(compositor->endFrame(gpuOut));

        const Difference difference = compare(cpuOut, gpuOut, 1);
        INFO(testCase.name << ": worst " << difference.worst << " at " << difference.worstX << ","
                           << difference.worstY << ", mean " << difference.mean);
        CHECK(difference.worst < 0.01F);
        CHECK(difference.mean < 0.002F);
    }
}

TEST_CASE("A grade on the GPU survives a fade without changing", "[gpu][golden][grade]") {
    // Premultiplied values: the shader has to divide alpha out before grading
    // and multiply it back after, or a clip would grade differently in the
    // middle of a dissolve than either side of it.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    model::ColorCorrection correction;
    correction.exposure = 0.5;
    correction.contrast = 35.0;
    correction.saturation = 140.0;
    const auto grade = render::gradeConstantsFor(correction);

    RgbaImage source{16, 16};
    source.fill(Rgba{0.4F, 0.25F, 0.1F, 1.0F});

    Transform fading;
    fading.opacity = 0.35;

    ZARO_REQUIRE_OK(compositor->beginFrame(16, 16));
    ZARO_REQUIRE_OK(compositor->draw(source, fading, BlendMode::Normal, grade));
    RgbaImage faded;
    ZARO_REQUIRE_OK(compositor->endFrame(faded));

    ZARO_REQUIRE_OK(compositor->beginFrame(16, 16));
    ZARO_REQUIRE_OK(compositor->draw(source, Transform{}, BlendMode::Normal, grade));
    RgbaImage full;
    ZARO_REQUIRE_OK(compositor->endFrame(full));

    const Rgba& partial = faded.at(8, 8);
    const Rgba& opaque = full.at(8, 8);
    REQUIRE(partial.a > 0.3F);
    CHECK(partial.r / partial.a == Catch::Approx(opaque.r).margin(0.005));
    CHECK(partial.g / partial.a == Catch::Approx(opaque.g).margin(0.005));
    CHECK(partial.b / partial.a == Catch::Approx(opaque.b).margin(0.005));
}

TEST_CASE("The GPU tone curve agrees with the CPU reference", "[gpu][golden][curves]") {
    // The curve is defined on the CPU, baked on the CPU, and the shader only
    // looks the answer up. This test is what says the lookup -- the index
    // function, the texture format, the filtering -- does not change it.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    const auto through = [](std::initializer_list<model::CurvePoint> points) {
        model::ToneCurve curve;
        for (const model::CurvePoint& point : points) {
            curve.set(point);
        }
        return curve;
    };

    struct Case {
        const char* name;
        model::ToneCurves curves;
    };
    std::vector<Case> cases;
    {
        model::ToneCurves lift;
        lift.master = through({{0.0, 0.1}, {0.5, 0.55}, {1.0, 1.0}});
        cases.push_back({"lifted blacks", lift});

        model::ToneCurves sCurve;
        sCurve.master = through({{0.0, 0.0}, {0.25, 0.15}, {0.75, 0.85}, {1.0, 1.0}});
        cases.push_back({"s-curve", sCurve});

        model::ToneCurves split;
        split.red = through({{0.0, 0.05}, {1.0, 1.0}});
        split.blue = through({{0.0, 0.0}, {1.0, 0.9}});
        cases.push_back({"warm split tone", split});

        model::ToneCurves everything;
        everything.master = through({{0.0, 0.02}, {0.5, 0.6}, {1.0, 0.98}});
        everything.red = through({{0.0, 0.0}, {0.4, 0.5}, {1.0, 1.0}});
        everything.green = through({{0.0, 0.0}, {0.6, 0.5}, {1.0, 1.0}});
        everything.blue = through({{0.0, 0.05}, {1.0, 0.95}});
        cases.push_back({"all four", everything});
    }

    RgbaImage source{32, 32};
    for (std::int32_t y = 0; y < 32; ++y) {
        Rgba* row = source.row(y);
        for (std::int32_t x = 0; x < 32; ++x) {
            const float u = static_cast<float>(x) / 31.0F;
            const float v = static_cast<float>(y) / 31.0F;
            row[x] = Rgba{u * 1.4F, v, (1.0F - u) * 0.8F, 1.0F};
        }
    }

    for (const Case& testCase : cases) {
        const render::CurveTable table{testCase.curves, media::TransferFunction::BT709};
        REQUIRE_FALSE(table.isIdentity());
        const render::GradeConstants neutral;

        RgbaImage cpuOut{32, 32};
        render::drawTransformed(source, cpuOut, Transform{}, BlendMode::Normal,
                                {.grade = &neutral, .curves = &table});

        ZARO_REQUIRE_OK(compositor->beginFrame(32, 32));
        ZARO_REQUIRE_OK(compositor->draw(source, Transform{}, BlendMode::Normal, neutral, &table));
        RgbaImage gpuOut;
        ZARO_REQUIRE_OK(compositor->endFrame(gpuOut));

        const Difference difference = compare(cpuOut, gpuOut, 1);
        INFO(testCase.name << ": worst " << difference.worst << " at " << difference.worstX << ","
                           << difference.worstY << ", mean " << difference.mean);
        CHECK(difference.worst < 0.01F);
        CHECK(difference.mean < 0.002F);
    }
}

TEST_CASE("An identity curve leaves the GPU picture untouched", "[gpu][golden][curves]") {
    // Sampling an identity table would round every ungraded pixel through the
    // table's own resolution. An ungraded clip has to come back exactly as it
    // went in, which is what the frame-exact harness depends on.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    RgbaImage source{16, 16};
    for (std::int32_t y = 0; y < 16; ++y) {
        Rgba* row = source.row(y);
        for (std::int32_t x = 0; x < 16; ++x) {
            row[x] =
                Rgba{static_cast<float>(x) / 15.0F, static_cast<float>(y) / 15.0F, 0.37F, 1.0F};
        }
    }

    const render::CurveTable identity{model::ToneCurves{}, media::TransferFunction::BT709};
    REQUIRE(identity.isIdentity());

    ZARO_REQUIRE_OK(compositor->beginFrame(16, 16));
    ZARO_REQUIRE_OK(compositor->draw(source, Transform{}, BlendMode::Normal,
                                     render::GradeConstants{}, &identity));
    RgbaImage out;
    ZARO_REQUIRE_OK(compositor->endFrame(out));

    for (std::int32_t y = 0; y < 16; ++y) {
        for (std::int32_t x = 0; x < 16; ++x) {
            REQUIRE(out.at(x, y).r == Catch::Approx(source.at(x, y).r).margin(1e-6));
            REQUIRE(out.at(x, y).g == Catch::Approx(source.at(x, y).g).margin(1e-6));
            REQUIRE(out.at(x, y).b == Catch::Approx(source.at(x, y).b).margin(1e-6));
        }
    }
}

TEST_CASE("The GPU YUV path honours a tone curve", "[gpu][golden][curves][yuv]") {
    // drawSource is a different code path from draw: the planes are converted
    // in one pass and composited in another, and only the second one carries
    // the grade. A curve that works through draw and not through drawSource
    // would be invisible in preview for every real clip, since every real clip
    // arrives as YUV.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    const media::VideoFrame frame = yuvPattern(
        64, 64, media::PixelFormat::YUV420P, media::ColorRange::Limited, media::ColorMatrix::BT709);

    model::ToneCurves curves;
    curves.master.set({0.0, 0.25});
    curves.master.set({0.5, 0.7});
    curves.master.set({1.0, 1.0});
    const render::CurveTable table{curves, media::TransferFunction::BT709};
    REQUIRE_FALSE(table.isIdentity());

    RgbaImage plain;
    ZARO_REQUIRE_OK(compositor->beginFrame(64, 64));
    ZARO_REQUIRE_OK(
        compositor->drawSource(frame, Transform{}, render::GradeConstants{}, BlendMode::Normal));
    ZARO_REQUIRE_OK(compositor->endFrame(plain));

    RgbaImage curved;
    ZARO_REQUIRE_OK(compositor->beginFrame(64, 64));
    ZARO_REQUIRE_OK(compositor->drawSource(frame, Transform{}, render::GradeConstants{},
                                           BlendMode::Normal, &table));
    ZARO_REQUIRE_OK(compositor->endFrame(curved));

    // The curve lifts black a long way, so every pixel should have moved.
    double moved = 0.0;
    for (std::int32_t y = 0; y < 64; ++y) {
        for (std::int32_t x = 0; x < 64; ++x) {
            moved += static_cast<double>(std::fabs(curved.at(x, y).r - plain.at(x, y).r));
        }
    }
    INFO("total change across the frame: " << moved);
    CHECK(moved > 10.0);

    // And it agrees with the CPU doing the same thing to the same frame.
    RgbaImage converted;
    ZARO_REQUIRE_OK(render::toLinear(frame, converted));
    RgbaImage cpuOut{64, 64};
    const render::GradeConstants neutral;
    render::drawTransformed(converted, cpuOut, Transform{}, BlendMode::Normal,
                            {.grade = &neutral, .curves = &table});

    const Difference difference = compare(cpuOut, curved, 1);
    INFO("worst " << difference.worst << ", mean " << difference.mean);
    CHECK(difference.worst < 0.02F);
}

TEST_CASE("The GPU secondary agrees with the CPU reference", "[gpu][golden][secondary]") {
    // The qualifier is the one piece of this pipeline written twice: the CPU
    // has render::qualifierMask and the shader has its own copy, because a
    // per-pixel mask cannot be baked into a table the way a curve can. So it
    // gets the same treatment the primary correction did -- checked directly,
    // over enough cases that a disagreement in any one window shows up.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    struct Case {
        const char* name;
        model::Secondary secondary;
    };
    std::vector<Case> cases;
    {
        model::Secondary reds;
        reds.qualifier.enabled = true;
        reds.qualifier.hueCentre = 0.0;
        reds.qualifier.hueWidth = 60.0;
        reds.qualifier.hueSoftness = 20.0;
        reds.correction.exposure = -1.5;
        cases.push_back({"reds down", reds});

        model::Secondary greens;
        greens.qualifier.enabled = true;
        greens.qualifier.hueCentre = 120.0;
        greens.qualifier.hueWidth = 80.0;
        greens.qualifier.hueSoftness = 30.0;
        greens.correction.saturation = 170.0;
        cases.push_back({"greens up", greens});

        model::Secondary shadows;
        shadows.qualifier.enabled = true;
        shadows.qualifier.lumaHigh = 0.35;
        shadows.qualifier.lumaSoftness = 0.1;
        shadows.correction.temperature = 60.0;
        cases.push_back({"cool shadows", shadows});

        model::Secondary highs;
        highs.qualifier.enabled = true;
        highs.qualifier.lumaLow = 0.6;
        highs.qualifier.lumaSoftness = 0.1;
        highs.correction.contrast = 40.0;
        cases.push_back({"highlight contrast", highs});

        model::Secondary vivid;
        vivid.qualifier.enabled = true;
        vivid.qualifier.saturationLow = 0.4;
        vivid.qualifier.saturationSoftness = 0.15;
        vivid.correction.saturation = 40.0;
        cases.push_back({"tame the vivid", vivid});

        model::Secondary masked;
        masked.qualifier.enabled = true;
        masked.qualifier.hueCentre = 200.0;
        masked.qualifier.hueWidth = 70.0;
        masked.qualifier.hueSoftness = 25.0;
        masked.showMask = true;
        cases.push_back({"mask view", masked});
    }

    // Every hue, a spread of saturations, and brightnesses from shadow to
    // above white -- so each window is exercised by something.
    RgbaImage source{48, 48};
    for (std::int32_t y = 0; y < 48; ++y) {
        Rgba* row = source.row(y);
        for (std::int32_t x = 0; x < 48; ++x) {
            const float hue = (static_cast<float>(x) / 48.0F) * 6.0F;
            const auto sector = static_cast<int>(hue) % 6;
            const float f = hue - std::floor(hue);
            const float level = 0.02F + ((static_cast<float>(y) / 47.0F) * 1.3F);
            const float dull = static_cast<float>(y % 3) * 0.25F;
            float r = 0.0F;
            float g = 0.0F;
            float b = 0.0F;
            switch (sector) {
                case 0:
                    r = 1.0F;
                    g = f;
                    b = 0.0F;
                    break;
                case 1:
                    r = 1.0F - f;
                    g = 1.0F;
                    b = 0.0F;
                    break;
                case 2:
                    r = 0.0F;
                    g = 1.0F;
                    b = f;
                    break;
                case 3:
                    r = 0.0F;
                    g = 1.0F - f;
                    b = 1.0F;
                    break;
                case 4:
                    r = f;
                    g = 0.0F;
                    b = 1.0F;
                    break;
                default:
                    r = 1.0F;
                    g = 0.0F;
                    b = 1.0F - f;
                    break;
            }
            row[x] =
                Rgba{((r * (1.0F - dull)) + dull) * level, ((g * (1.0F - dull)) + dull) * level,
                     ((b * (1.0F - dull)) + dull) * level, 1.0F};
        }
    }

    for (const Case& testCase : cases) {
        const auto secondary =
            render::secondaryConstantsFor(testCase.secondary, media::TransferFunction::BT709);
        REQUIRE(secondary.isActive());
        const render::GradeConstants neutral;

        RgbaImage cpuOut{48, 48};
        render::drawTransformed(source, cpuOut, Transform{}, BlendMode::Normal,
                                {.grade = &neutral, .secondary = &secondary});

        ZARO_REQUIRE_OK(compositor->beginFrame(48, 48));
        ZARO_REQUIRE_OK(
            compositor->draw(source, Transform{}, BlendMode::Normal, neutral, nullptr, &secondary));
        RgbaImage gpuOut;
        ZARO_REQUIRE_OK(compositor->endFrame(gpuOut));

        const Difference difference = compare(cpuOut, gpuOut, 1);
        INFO(testCase.name << ": worst " << difference.worst << " at " << difference.worstX << ","
                           << difference.worstY << ", mean " << difference.mean);
        CHECK(difference.worst < 0.02F);
        CHECK(difference.mean < 0.002F);
    }
}

TEST_CASE("The GPU look LUT agrees with the CPU reference", "[gpu][golden][lut]") {
    // The cube is baked once on the CPU and sampled by both, so what this test
    // is really checking is the parts that differ: the 3D texture upload, the
    // hardware's trilinear filtering against the CPU's, and the un-warp that
    // turns a stored index back into light.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    const auto cubeText = [](int size, int mode) {
        std::ostringstream out;
        out << "LUT_3D_SIZE " << size << "\n";
        for (int b = 0; b < size; ++b) {
            for (int g = 0; g < size; ++g) {
                for (int r = 0; r < size; ++r) {
                    const double rr = static_cast<double>(r) / (size - 1);
                    const double gg = static_cast<double>(g) / (size - 1);
                    const double bb = static_cast<double>(b) / (size - 1);
                    if (mode == 0) {  // identity
                        out << rr << " " << gg << " " << bb << "\n";
                    } else if (mode == 1) {  // a warm, contrasty look
                        out << std::min(1.0, rr * 1.15) << " " << (gg * gg) << " " << (bb * 0.8)
                            << "\n";
                    } else {  // channel swap, which no symmetric look would catch
                        out << bb << " " << gg << " " << rr << "\n";
                    }
                }
            }
        }
        return out.str();
    };

    RgbaImage source{32, 32};
    for (std::int32_t y = 0; y < 32; ++y) {
        Rgba* row = source.row(y);
        for (std::int32_t x = 0; x < 32; ++x) {
            const float u = static_cast<float>(x) / 31.0F;
            const float v = static_cast<float>(y) / 31.0F;
            row[x] = Rgba{u * 1.2F, v, (1.0F - u) * 0.7F, 1.0F};
        }
    }

    struct Case {
        const char* name;
        int mode;
        float amount;
    };
    for (const Case& testCase :
         {Case{"identity", 0, 1.0F}, Case{"warm look", 1, 1.0F}, Case{"warm look at half", 1, 0.5F},
          Case{"channel swap", 2, 1.0F}}) {
        const auto cube = io::CubeLut::parse(cubeText(17, testCase.mode));
        REQUIRE(cube);
        const render::LutTable table{*cube, media::TransferFunction::BT709};
        REQUIRE(table.isValid());
        const render::GradeConstants neutral;

        RgbaImage cpuOut{32, 32};
        render::drawTransformed(source, cpuOut, Transform{}, BlendMode::Normal,
                                {.grade = &neutral, .lut = &table, .lutAmount = testCase.amount});

        ZARO_REQUIRE_OK(compositor->beginFrame(32, 32));
        ZARO_REQUIRE_OK(compositor->draw(source, Transform{}, BlendMode::Normal, neutral, nullptr,
                                         nullptr, &table, testCase.amount));
        RgbaImage gpuOut;
        ZARO_REQUIRE_OK(compositor->endFrame(gpuOut));

        const Difference difference = compare(cpuOut, gpuOut, 1);
        INFO(testCase.name << ": worst " << difference.worst << " at " << difference.worstX << ","
                           << difference.worstY << ", mean " << difference.mean);
        CHECK(difference.worst < 0.02F);
        CHECK(difference.mean < 0.004F);
    }
}

TEST_CASE("The GPU mask agrees with the CPU reference", "[gpu][golden][mask]") {
    // The mask geometry is written twice -- a signed distance cannot be baked
    // into a table the way a curve can, since it depends on where the fragment
    // lands. So it gets the same treatment as the qualifier: compared directly,
    // over enough cases that a disagreement in any part shows up.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    struct Case {
        const char* name;
        model::Mask mask;
    };
    const auto make = [](model::MaskShape shape, double w, double h, double cx, double cy,
                         double corner, double feather, bool inverted) {
        model::Mask mask;
        mask.shape = shape;
        mask.width = w;
        mask.height = h;
        mask.centreX = cx;
        mask.centreY = cy;
        mask.cornerRadius = corner;
        mask.feather = feather;
        mask.inverted = inverted;
        return mask;
    };
    const Case cases[] = {
        {"centred rectangle", make(model::MaskShape::Rectangle, 30, 20, 0, 0, 0, 0, false)},
        {"offset rectangle", make(model::MaskShape::Rectangle, 24, 24, 10, -8, 0, 0, false)},
        {"rounded", make(model::MaskShape::Rectangle, 36, 28, 0, 0, 8, 0, false)},
        {"feathered", make(model::MaskShape::Rectangle, 30, 20, 0, 0, 0, 9, false)},
        {"ellipse", make(model::MaskShape::Ellipse, 40, 24, -4, 6, 0, 0, false)},
        {"inverted ellipse", make(model::MaskShape::Ellipse, 30, 30, 0, 0, 0, 5, true)},
    };

    RgbaImage source{64, 64};
    source.fill(Rgba{0.7F, 0.5F, 0.3F, 1.0F});

    for (const Case& testCase : cases) {
        RgbaImage cpuOut{64, 64};
        render::drawTransformed(source, cpuOut, Transform{}, BlendMode::Normal,
                                {.mask = &testCase.mask});

        ZARO_REQUIRE_OK(compositor->beginFrame(64, 64));
        ZARO_REQUIRE_OK(compositor->draw(source, Transform{}, BlendMode::Normal,
                                         render::GradeConstants{}, nullptr, nullptr, nullptr, 1.0F,
                                         &testCase.mask));
        RgbaImage gpuOut;
        ZARO_REQUIRE_OK(compositor->endFrame(gpuOut));

        const Difference difference = compare(cpuOut, gpuOut, 1);
        INFO(testCase.name << ": worst " << difference.worst << " at " << difference.worstX << ","
                           << difference.worstY << ", mean " << difference.mean);
        CHECK(difference.worst < 0.02F);
        CHECK(difference.mean < 0.002F);
    }
}

TEST_CASE("The GPU keyer agrees with the CPU reference", "[gpu][golden][keyer]") {
    // The one effect that changes *alpha* rather than colour, so a disagreement
    // between the two paths shows up as an edge that is soft in preview and
    // hard in the export -- or the other way round, which is worse because the
    // export is the one nobody watches all the way through.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    struct Case {
        const char* name;
        model::Keyer keyer;
    };
    std::vector<Case> cases;
    {
        model::Keyer green;
        green.kind = model::KeyKind::Chroma;
        green.red = 0.05;
        green.green = 0.85;
        green.blue = 0.1;
        green.tolerance = 0.12;
        green.softness = 0.08;
        green.spill = 1.0;
        cases.push_back({"green screen", green});

        model::Keyer blue = green;
        blue.red = 0.05;
        blue.green = 0.1;
        blue.blue = 0.85;
        blue.spill = 0.5;
        cases.push_back({"blue screen, half spill", blue});

        model::Keyer wide = green;
        wide.tolerance = 0.30;
        wide.softness = 0.35;
        wide.spill = 0.0;
        cases.push_back({"a wide, soft key", wide});

        model::Keyer luma;
        luma.kind = model::KeyKind::Luma;
        luma.lumaLow = 0.0;
        luma.lumaHigh = 0.35;
        luma.lumaSoftness = 0.12;
        cases.push_back({"drop the shadows", luma});

        model::Keyer matte = green;
        matte.showMatte = true;
        cases.push_back({"matte view", matte});
    }

    // Every hue, a spread of saturations, and brightnesses from shadow to above
    // white, so the chromaticity divide is exercised near zero as well as in
    // the middle.
    RgbaImage source{48, 48};
    for (std::int32_t y = 0; y < 48; ++y) {
        Rgba* row = source.row(y);
        for (std::int32_t x = 0; x < 48; ++x) {
            const float hue = (static_cast<float>(x) / 48.0F) * 6.0F;
            const auto sector = static_cast<int>(hue) % 6;
            const float f = hue - std::floor(hue);
            const float level = 0.001F + ((static_cast<float>(y) / 47.0F) * 1.2F);
            const float dull = static_cast<float>(y % 3) * 0.25F;
            float r = 0.0F;
            float g = 0.0F;
            float b = 0.0F;
            switch (sector) {
                case 0:
                    r = 1.0F;
                    g = f;
                    break;
                case 1:
                    r = 1.0F - f;
                    g = 1.0F;
                    break;
                case 2:
                    g = 1.0F;
                    b = f;
                    break;
                case 3:
                    g = 1.0F - f;
                    b = 1.0F;
                    break;
                case 4:
                    r = f;
                    b = 1.0F;
                    break;
                default:
                    r = 1.0F;
                    b = 1.0F - f;
                    break;
            }
            row[x] =
                Rgba{((r * (1.0F - dull)) + dull) * level, ((g * (1.0F - dull)) + dull) * level,
                     ((b * (1.0F - dull)) + dull) * level, 1.0F};
        }
    }

    for (const Case& testCase : cases) {
        const auto keyer =
            render::keyerConstantsFor(testCase.keyer, media::TransferFunction::BT709);
        REQUIRE(keyer.isActive());
        const render::GradeConstants neutral;

        RgbaImage cpuOut{48, 48};
        render::drawTransformed(source, cpuOut, Transform{}, BlendMode::Normal,
                                {.grade = &neutral, .keyer = &keyer});

        ZARO_REQUIRE_OK(compositor->beginFrame(48, 48));
        ZARO_REQUIRE_OK(compositor->draw(source, Transform{}, BlendMode::Normal, neutral, nullptr,
                                         nullptr, nullptr, 1.0F, nullptr, &keyer));
        RgbaImage gpuOut;
        ZARO_REQUIRE_OK(compositor->endFrame(gpuOut));

        const Difference difference = compare(cpuOut, gpuOut, 1);
        INFO(testCase.name << ": worst " << difference.worst << " at " << difference.worstX << ","
                           << difference.worstY << ", mean " << difference.mean);
        CHECK(difference.worst < 0.02F);
        CHECK(difference.mean < 0.002F);
    }
}

TEST_CASE("Switching output size and source size does not wreck the pipeline",
          "[gpu][drawsource][regression]") {
    // A pipeline outliving the render pass descriptor it was built against.
    //
    // The intermediate pass descriptor used to be a raw pointer into whichever
    // staging slot happened to create the first one. Two things then had to
    // line up, and opening a second project lines them up every time: a change
    // of output size clears the conversion pipeline so it will be rebuilt, and
    // a change of *source* size makes the staging slot destroy and recreate its
    // descriptor. The rebuilt pipeline was then handed a pointer to freed
    // memory, and Metal aborted the process reporting a colour attachment with
    // an invalid pixel format -- from inside an ordinary repaint, with nothing
    // in the stack to say why.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    const media::VideoFrame small = yuvPattern(
        64, 48, media::PixelFormat::YUV420P, media::ColorRange::Limited, media::ColorMatrix::BT709);
    const media::VideoFrame large = yuvPattern(
        96, 64, media::PixelFormat::YUV420P, media::ColorRange::Limited, media::ColorMatrix::BT709);

    ZARO_REQUIRE_OK(compositor->beginFrame(64, 48));
    ZARO_REQUIRE_OK(compositor->drawSource(small, Transform{}, render::GradeConstants{}));
    RgbaImage first;
    ZARO_REQUIRE_OK(compositor->endFrame(first));

    // A different output size clears the conversion pipeline; a different
    // source size recreates the staging surface it was built against.
    ZARO_REQUIRE_OK(compositor->beginFrame(96, 64));
    ZARO_REQUIRE_OK(compositor->drawSource(large, Transform{}, render::GradeConstants{}));
    RgbaImage second;
    ZARO_REQUIRE_OK(compositor->endFrame(second));
    CHECK(second.width() == 96);

    // And back again, which is what closing one project and opening another
    // looks like from here.
    ZARO_REQUIRE_OK(compositor->beginFrame(64, 48));
    ZARO_REQUIRE_OK(compositor->drawSource(small, Transform{}, render::GradeConstants{}));
    RgbaImage third;
    ZARO_REQUIRE_OK(compositor->endFrame(third));
    CHECK(third.width() == 64);
}

TEST_CASE("The GPU agrees with the CPU on log and HDR curves", "[gpu][golden][log]") {
    // These curves are the reason the two implementations exist separately: the
    // CPU samples a table because std::pow per pixel is ruinous, and the shader
    // evaluates the formula because a GPU does that for free. A log curve that
    // is right in one and wrong in the other produces an export that does not
    // match the preview it was graded in -- and log footage is precisely the
    // footage nobody can eyeball, because it is meant to look flat.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    struct Case {
        const char* name;
        media::TransferFunction transfer;
    };
    const Case cases[] = {
        {"S-Log3", media::TransferFunction::SLog3}, {"V-Log", media::TransferFunction::VLog},
        {"LogC3", media::TransferFunction::LogC3},  {"PQ", media::TransferFunction::PQ},
        {"HLG", media::TransferFunction::HLG},
    };

    for (const Case& testCase : cases) {
        media::VideoFrame frame = yuvPattern(64, 64, media::PixelFormat::YUV420P,
                                             media::ColorRange::Limited, media::ColorMatrix::BT709);
        media::ColorInfo color = frame.color();
        color.transfer = testCase.transfer;
        frame.setColor(color);

        RgbaImage converted;
        ZARO_REQUIRE_OK(render::toLinear(frame, converted));
        RgbaImage cpuOut{64, 64};
        render::drawTransformed(converted, cpuOut, Transform{}, BlendMode::Normal);

        ZARO_REQUIRE_OK(compositor->beginFrame(64, 64));
        ZARO_REQUIRE_OK(compositor->drawSource(frame, Transform{}, render::GradeConstants{},
                                               BlendMode::Normal));
        RgbaImage gpuOut;
        ZARO_REQUIRE_OK(compositor->endFrame(gpuOut));

        const Difference difference = compare(cpuOut, gpuOut, 1);
        INFO(testCase.name << ": worst " << difference.worst << " at " << difference.worstX << ","
                           << difference.worstY << ", mean " << difference.mean);
        // A looser bound than the display curves get, and honestly so: these
        // carry values far above 1.0, so the table's interpolation error is
        // scaled up with them. Relative to the values involved it is the same
        // error.
        CHECK(difference.worst < 0.05F);
        CHECK(difference.mean < 0.005F);
    }
}

TEST_CASE("The presented frame rolls off its highlights like the encoder does",
          "[gpu][golden][tonemap]") {
    // The divergence this closes: before it, a graded highlight looked clipped
    // on screen and rolled off in the file. Every parity test in this project
    // exists to stop preview and export disagreeing, and adding a rolloff to
    // one and not the other created exactly that.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    // A ramp that runs well past white, so most of it is above any knee.
    RgbaImage source{32, 1};
    for (std::int32_t x = 0; x < 32; ++x) {
        const auto value = static_cast<float>(x) / 4.0F;  // 0 .. 7.75
        source.at(x, 0) = Rgba{value, value, value, 1.0F};
    }

    constexpr double knee = 0.7;
    ZARO_REQUIRE_OK(compositor->beginFrame(32, 1));
    ZARO_REQUIRE_OK(compositor->draw(source, Transform{}, BlendMode::Normal));
    ZARO_REQUIRE_OK(compositor->endFrameOnGpu());
    compositor->setPresentKnee(knee);
    RgbaImage presented;
    ZARO_REQUIRE_OK(compositor->presentToImage(32, 1, presented));

    // The reference: the same rolloff the encoder applies, on the CPU.
    RgbaImage expected = source.clone();
    render::toneMap(expected, static_cast<float>(knee));

    float worst = 0.0F;
    for (std::int32_t x = 0; x < 32; ++x) {
        worst = std::max(worst, std::abs(presented.at(x, 0).g - expected.at(x, 0).g));
    }
    INFO("worst difference " << worst);
    CHECK(worst < 0.01F);
    // And it actually did something: the top of the ramp is below white rather
    // than clipped to it.
    CHECK(presented.at(31, 0).g < 1.0F);
    CHECK(presented.at(31, 0).g > presented.at(24, 0).g);

    SECTION("and with no rolloff the ramp clips, as it always did") {
        compositor->setPresentKnee(1.0);
        RgbaImage plain;
        ZARO_REQUIRE_OK(compositor->presentToImage(32, 1, plain));
        CHECK(plain.at(31, 0).g >= 0.99F);
    }
}

TEST_CASE("The GPU agrees with the CPU on the colour wheels", "[gpu][golden][wheels]") {
    // The CDL is three pow() calls per pixel on both sides, and a power is
    // exactly the kind of arithmetic where two implementations drift -- one
    // clamping at zero and the other not, one applying the wheels before
    // contrast and the other after. A grade that looked right in the preview
    // and wrong in the file would be the result.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    struct Case {
        const char* name;
        model::ColorWheels wheels;
    };
    std::vector<Case> cases;
    {
        model::ColorWheels warmShadows;
        warmShadows.offsetR = 0.04;
        warmShadows.offsetB = -0.03;
        cases.push_back({"warm the shadows", warmShadows});

        model::ColorWheels coolHighlights;
        coolHighlights.slopeR = 0.9;
        coolHighlights.slopeB = 1.15;
        cases.push_back({"cool the highlights", coolHighlights});

        model::ColorWheels midtones;
        midtones.powerR = 0.85;
        midtones.powerG = 0.95;
        midtones.powerB = 1.1;
        cases.push_back({"twist the midtones", midtones});

        model::ColorWheels crushing;
        crushing.offsetR = crushing.offsetG = crushing.offsetB = -0.06;
        crushing.powerR = crushing.powerG = crushing.powerB = 0.6;
        cases.push_back({"crush into the toe", crushing});
    }

    // Shadows through to well above white, so the clamp at zero and the
    // behaviour past one are both exercised.
    RgbaImage source{48, 48};
    for (std::int32_t y = 0; y < 48; ++y) {
        Rgba* row = source.row(y);
        for (std::int32_t x = 0; x < 48; ++x) {
            const float level = 0.001F + ((static_cast<float>(y) / 47.0F) * 1.4F);
            const float tint = static_cast<float>(x) / 47.0F;
            row[x] = Rgba{level * (0.4F + tint), level, level * (1.4F - tint), 1.0F};
        }
    }

    for (const Case& testCase : cases) {
        const auto grade = render::gradeConstantsFor(model::ColorCorrection{}, testCase.wheels);
        REQUIRE(grade.wheels);

        RgbaImage cpuOut{48, 48};
        render::drawTransformed(source, cpuOut, Transform{}, BlendMode::Normal, {.grade = &grade});

        ZARO_REQUIRE_OK(compositor->beginFrame(48, 48));
        ZARO_REQUIRE_OK(compositor->draw(source, Transform{}, BlendMode::Normal, grade));
        RgbaImage gpuOut;
        ZARO_REQUIRE_OK(compositor->endFrame(gpuOut));

        const Difference difference = compare(cpuOut, gpuOut, 1);
        INFO(testCase.name << ": worst " << difference.worst << " at " << difference.worstX << ","
                           << difference.worstY << ", mean " << difference.mean);
        CHECK(difference.worst < 0.02F);
        CHECK(difference.mean < 0.002F);
    }
}

TEST_CASE("The GPU agrees with the CPU on the vignette", "[gpu][golden][vignette]") {
    // Geometry evaluated per pixel in two places, which is how a mask once came
    // to feather differently on the two paths. The falloff shape is shared with
    // the qualifier and the mask, so a disagreement here would be one there too.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    struct Case {
        const char* name;
        model::Vignette vignette;
    };
    std::vector<Case> cases;
    {
        model::Vignette classic;
        classic.amount = -0.6;
        cases.push_back({"an ordinary vignette", classic});

        model::Vignette round;
        round.amount = -0.8;
        round.roundness = 0.0;
        round.midpoint = 0.4;
        cases.push_back({"round like a lens", round});

        model::Vignette tight;
        tight.amount = -1.0;
        tight.midpoint = 0.2;
        tight.feather = 0.05;
        cases.push_back({"a hard edge", tight});

        model::Vignette lifted;
        lifted.amount = 0.5;
        cases.push_back({"lifting the corners instead", lifted});
    }

    RgbaImage source{64, 36};  // deliberately not square, so roundness matters
    for (std::int32_t y = 0; y < 36; ++y) {
        Rgba* row = source.row(y);
        for (std::int32_t x = 0; x < 64; ++x) {
            const float level = 0.2F + (static_cast<float>(x) / 128.0F);
            row[x] = Rgba{level, level * 0.8F, level * 0.6F, 1.0F};
        }
    }

    for (const Case& testCase : cases) {
        RgbaImage cpuOut{64, 36};
        render::drawTransformed(source, cpuOut, Transform{}, BlendMode::Normal,
                                {.vignette = &testCase.vignette});

        ZARO_REQUIRE_OK(compositor->beginFrame(64, 36));
        ZARO_REQUIRE_OK(compositor->draw(source, Transform{}, BlendMode::Normal,
                                         render::GradeConstants{}, nullptr, nullptr, nullptr, 1.0F,
                                         nullptr, nullptr, &testCase.vignette));
        RgbaImage gpuOut;
        ZARO_REQUIRE_OK(compositor->endFrame(gpuOut));

        const Difference difference = compare(cpuOut, gpuOut, 1);
        INFO(testCase.name << ": worst " << difference.worst << " at " << difference.worstX << ","
                           << difference.worstY << ", mean " << difference.mean);
        CHECK(difference.worst < 0.01F);
        CHECK(difference.mean < 0.002F);
    }
}

TEST_CASE("The GPU agrees with the CPU on a wipe", "[gpu][golden][transition]") {
    // A wipe is a second mask multiplied into the first, which is a change to
    // the one function both paths use for coverage. Getting it wrong on one
    // side is a transition that plays differently in the export than in the
    // preview -- and a wipe is the transition where that is most visible,
    // because the boundary is a hard edge somebody can point at.
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    // Named on every failure: which backend a test ran on is the first thing
    // anybody reading a red CI log needs, and it is the one thing the machine
    // knows that we do not.
    INFO("backend: " << compositor->backendName());

    model::Transition transition;
    transition.kind = model::TransitionKind::Wipe;

    RgbaImage source{64, 36};
    source.fill(Rgba{0.6F, 0.3F, 0.15F, 1.0F});

    for (const model::TransitionDirection direction :
         {model::TransitionDirection::Right, model::TransitionDirection::Left,
          model::TransitionDirection::Down, model::TransitionDirection::Up}) {
        transition.direction = direction;
        for (const double progress : {0.0, 0.25, 0.5, 0.75, 1.0}) {
            const render::TransitionShape shape =
                render::transitionShapeFor(transition, progress, 64, 36);

            RgbaImage cpuOut{64, 36};
            render::drawTransformed(source, cpuOut, Transform{}, BlendMode::Normal,
                                    {.wipe = shape.wipe.isSet() ? &shape.wipe : nullptr});

            ZARO_REQUIRE_OK(compositor->beginFrame(64, 36));
            ZARO_REQUIRE_OK(compositor->draw(source, Transform{}, BlendMode::Normal,
                                             render::GradeConstants{}, nullptr, nullptr, nullptr,
                                             1.0F, nullptr, nullptr, nullptr,
                                             shape.wipe.isSet() ? &shape.wipe : nullptr));
            RgbaImage gpuOut;
            ZARO_REQUIRE_OK(compositor->endFrame(gpuOut));

            const Difference difference = compare(cpuOut, gpuOut, 1);
            INFO(model::toString(direction) << " at " << progress << ": worst " << difference.worst
                                            << ", mean " << difference.mean);
            CHECK(difference.worst < 0.01F);
        }
    }
}
