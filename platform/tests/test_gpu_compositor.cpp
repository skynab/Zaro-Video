#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/media/VideoFrame.h"
#include "zaro/core/render/ColorPipeline.h"
#include "zaro/core/render/Compositing.h"
#include "zaro/core/render/RenderGraph.h"
#include "zaro/platform/qrhi/GpuCompositor.h"
#include "zaro/platform/qrhi/GpuRenderGraph.h"

#include "ModelFixtures.h"

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

    REQUIRE(compositor.beginFrame(width, height).ok());
    if (background.a > 0.0F || background.r > 0.0F) {
        RgbaImage backdrop = filled(width, height, background);
        REQUIRE(compositor.draw(backdrop, Transform{}, BlendMode::Normal).ok());
    }
    REQUIRE(compositor.draw(source, transform, blend).ok());
    REQUIRE(compositor.endFrame(pair.gpu).ok());
    return pair;
}

}  // namespace

TEST_CASE("A GPU backend is available", "[gpu]") {
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    INFO("backend: " << compositor->backendName());
    CHECK_FALSE(compositor->backendName().empty());
}

TEST_CASE("An empty GPU frame is transparent", "[gpu]") {
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
    REQUIRE(compositor->beginFrame(16, 16).ok());
    RgbaImage out;
    REQUIRE(compositor->endFrame(out).ok());
    CHECK(out.width() == 16);
    CHECK(out.at(8, 8).a == Approx(0.0F).margin(1e-6));
}

TEST_CASE("GPU and CPU agree on an untransformed draw", "[gpu][golden]") {
    auto compositor = gpu();
    if (!compositor) {
        SKIP("no GPU backend on this machine");
    }
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
        REQUIRE(compositor->beginFrame(kWidth, kHeight).ok());
        REQUIRE(compositor->draw(source, transform, BlendMode::Normal).ok());
        REQUIRE(compositor->endFrame(gpuOut).ok());
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
        REQUIRE(render::toLinear(yuv, linear).ok());
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
        REQUIRE(render::toLinear(frame, converted).ok());
        RgbaImage cpuOut{64, 64};
        render::drawTransformed(converted, cpuOut, Transform{}, BlendMode::Normal);

        // Under test: hand the planes to the GPU and let the shader do both.
        REQUIRE(compositor->beginFrame(64, 64).ok());
        const auto drawn =
            compositor->drawSource(frame, Transform{}, render::GradeConstants{}, BlendMode::Normal);
        REQUIRE(drawn.ok());
        RgbaImage gpuOut;
        REQUIRE(compositor->endFrame(gpuOut).ok());

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
        REQUIRE(render::toLinear(frame, converted).ok());
        RgbaImage cpuOut{96, 96};
        render::drawTransformed(converted, cpuOut, transform, BlendMode::Normal);

        REQUIRE(compositor->beginFrame(96, 96).ok());
        REQUIRE(
            compositor->drawSource(frame, transform, render::GradeConstants{}, BlendMode::Normal)
                .ok());
        RgbaImage gpuOut;
        REQUIRE(compositor->endFrame(gpuOut).ok());
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
    REQUIRE(compositor->beginFrame(16, 16).ok());

    media::VideoFrame hdr = yuvPattern(16, 16, media::PixelFormat::YUV420P,
                                       media::ColorRange::Limited, media::ColorMatrix::BT709);
    media::ColorInfo pq = hdr.color();
    pq.transfer = media::TransferFunction::PQ;
    hdr.setColor(pq);

    const auto status =
        compositor->drawSource(hdr, Transform{}, render::GradeConstants{}, BlendMode::Normal);
    REQUIRE_FALSE(status.ok());
    CHECK(status.error().code() == ErrorCode::Unsupported);

    RgbaImage out;
    REQUIRE(compositor->endFrame(out).ok());
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
        REQUIRE(render::toLinear(frame, converted).ok());
        cpuOut.clear();
        render::drawTransformed(converted, cpuOut, transform, BlendMode::Normal);
    }
    const double cpuSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - cpuStart).count();

    // The GPU pipeline as a preview would run it: planes up, nothing back.
    const auto previewStart = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(compositor->beginFrame(kWidth, kHeight).ok());
        REQUIRE(
            compositor->drawSource(frame, transform, render::GradeConstants{}, BlendMode::Normal)
                .ok());
        REQUIRE(compositor->endFrameOnGpu().ok());
    }
    const double previewSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - previewStart).count();

    // And as an export would, which still has to read the result back.
    RgbaImage gpuOut;
    const auto readbackStart = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(compositor->beginFrame(kWidth, kHeight).ok());
        REQUIRE(
            compositor->drawSource(frame, transform, render::GradeConstants{}, BlendMode::Normal)
                .ok());
        REQUIRE(compositor->endFrame(gpuOut).ok());
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

    // Asymmetric on purpose: white across the top third, black below.
    RgbaImage source{32, 30};
    for (std::int32_t y = 0; y < 30; ++y) {
        for (std::int32_t x = 0; x < 32; ++x) {
            source.at(x, y) = y < 10 ? premultiplied(1.0F, 1.0F, 1.0F, 1.0F)
                                     : premultiplied(0.0F, 0.0F, 0.0F, 1.0F);
        }
    }

    REQUIRE(compositor->beginFrame(32, 30).ok());
    REQUIRE(compositor->draw(source, Transform{}, BlendMode::Normal).ok());
    REQUIRE(compositor->endFrameOnGpu().ok());

    SECTION("the top of the picture stays at the top") {
        RgbaImage presented;
        // Same aspect ratio, so there are no bars to complicate it.
        REQUIRE(compositor->presentToImage(64, 60, presented).ok());
        CHECK(presented.at(32, 5).r > 0.5F);   // near the top: white
        CHECK(presented.at(32, 50).r < 0.5F);  // near the bottom: black
    }

    SECTION("a wider target gets bars at the sides, not the top") {
        RgbaImage presented;
        REQUIRE(compositor->presentToImage(160, 60, presented).ok());
        // The frame is 32x30, so in a 160x60 target it occupies the middle
        // 64 pixels horizontally and the full height.
        CHECK(presented.at(4, 30).a > 0.5F);  // bar: opaque black backdrop
        CHECK(presented.at(4, 30).r < 0.1F);
        CHECK(presented.at(80, 5).r > 0.5F);    // picture, still white on top
        CHECK(presented.at(156, 30).r < 0.1F);  // bar on the other side
    }

    SECTION("a taller target gets bars above and below") {
        RgbaImage presented;
        REQUIRE(compositor->presentToImage(64, 200, presented).ok());
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
        REQUIRE(gpuGraph.compositeInto(f.sequence(), at, onGpu).ok());
        RgbaImage onCpu;
        REQUIRE(cpuGraph.compositeInto(f.sequence(), at, onCpu).ok());

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
                                grade.isIdentity() ? nullptr : &grade);

        REQUIRE(compositor->beginFrame(32, 32).ok());
        REQUIRE(compositor->draw(source, Transform{}, BlendMode::Normal, grade).ok());
        RgbaImage gpuOut;
        REQUIRE(compositor->endFrame(gpuOut).ok());

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

    model::ColorCorrection correction;
    correction.exposure = 0.5;
    correction.contrast = 35.0;
    correction.saturation = 140.0;
    const auto grade = render::gradeConstantsFor(correction);

    RgbaImage source{16, 16};
    source.fill(Rgba{0.4F, 0.25F, 0.1F, 1.0F});

    Transform fading;
    fading.opacity = 0.35;

    REQUIRE(compositor->beginFrame(16, 16).ok());
    REQUIRE(compositor->draw(source, fading, BlendMode::Normal, grade).ok());
    RgbaImage faded;
    REQUIRE(compositor->endFrame(faded).ok());

    REQUIRE(compositor->beginFrame(16, 16).ok());
    REQUIRE(compositor->draw(source, Transform{}, BlendMode::Normal, grade).ok());
    RgbaImage full;
    REQUIRE(compositor->endFrame(full).ok());

    const Rgba& partial = faded.at(8, 8);
    const Rgba& opaque = full.at(8, 8);
    REQUIRE(partial.a > 0.3F);
    CHECK(partial.r / partial.a == Catch::Approx(opaque.r).margin(0.005));
    CHECK(partial.g / partial.a == Catch::Approx(opaque.g).margin(0.005));
    CHECK(partial.b / partial.a == Catch::Approx(opaque.b).margin(0.005));
}
