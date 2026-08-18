#include "zaro/platform/qrhi/GpuCompositor.h"

#include <QFile>
#include <QImage>
#include <array>
#include <cstring>
#include <vector>

#include <QMatrix4x4>
#include <rhi/qrhi.h>

#include "zaro/core/render/ColorPipeline.h"

namespace zaro::platform::qrhi {
namespace {

using model::BlendMode;

/// A unit quad as two triangles.
constexpr std::array<float, 12> kQuad{
    -1.0F, -1.0F, 1.0F, -1.0F, -1.0F, 1.0F, -1.0F, 1.0F, 1.0F, -1.0F, 1.0F, 1.0F,
};

/// 64 bytes of matrix plus a vec4, which satisfies std140 alignment.
constexpr int kUniformBytes = 64 + 16;
/// The YUV shader adds two more vec4s of colour parameters.
constexpr int kYuvUniformBytes = 64 + 16 * 3;

QShader loadShader(const char* path) {
    QFile file(QString::fromUtf8(path));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QShader::fromSerialized(file.readAll());
}

/// Blend state matching the CPU reference exactly, on premultiplied values.
void configureBlend(QRhiGraphicsPipeline::TargetBlend& target, BlendMode mode) {
    target.enable = true;
    target.opColor = QRhiGraphicsPipeline::Add;
    target.opAlpha = QRhiGraphicsPipeline::Add;
    target.srcAlpha = QRhiGraphicsPipeline::One;
    target.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;

    switch (mode) {
        case BlendMode::Add:
            target.srcColor = QRhiGraphicsPipeline::One;
            target.dstColor = QRhiGraphicsPipeline::One;
            break;
        case BlendMode::Multiply:
            // src*dst + dst*(1-srcA)
            target.srcColor = QRhiGraphicsPipeline::DstColor;
            target.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            break;
        case BlendMode::Screen:
            // src + dst - src*dst
            target.srcColor = QRhiGraphicsPipeline::One;
            target.dstColor = QRhiGraphicsPipeline::OneMinusSrcColor;
            break;
        case BlendMode::Normal:
        default:
            // Porter-Duff `over` on premultiplied values.
            target.srcColor = QRhiGraphicsPipeline::One;
            target.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            break;
    }
}

}  // namespace

struct GpuCompositor::State {
    std::unique_ptr<QRhi> rhi;
    std::unique_ptr<QRhiBuffer> vertexBuffer;
    std::unique_ptr<QRhiSampler> sampler;
    QShader vertexShader;
    QShader fragmentShader;
    QShader yuvFragmentShader;

    // One pipeline per blend mode per source kind, built on first use.
    // Pipelines are expensive to create and cheap to keep.
    std::array<std::unique_ptr<QRhiGraphicsPipeline>, 4> pipelines;
    std::array<std::unique_ptr<QRhiShaderResourceBindings>, 4> bindingLayouts;
    std::array<std::unique_ptr<QRhiGraphicsPipeline>, 4> yuvPipelines;
    std::array<std::unique_ptr<QRhiShaderResourceBindings>, 4> yuvBindingLayouts;
    std::unique_ptr<QRhiSampler> chromaSampler;

    /// A linear-light staging surface for one source in one frame.
    ///
    /// Conversion has to finish before the transform samples anything, so the
    /// two cannot share a pass. These are pooled by draw slot and reused across
    /// frames: allocating a texture per clip per frame would cost more than the
    /// pass it serves.
    struct Intermediate {
        std::unique_ptr<QRhiTexture> texture;
        std::unique_ptr<QRhiTextureRenderTarget> target;
        std::unique_ptr<QRhiRenderPassDescriptor> pass;
        QSize size;
    };
    std::vector<Intermediate> intermediates;
    std::size_t intermediateIndex{0};
    QRhiRenderPassDescriptor* intermediatePass{nullptr};

    std::unique_ptr<QRhiTexture> target;
    std::unique_ptr<QRhiTextureRenderTarget> renderTarget;
    std::unique_ptr<QRhiRenderPassDescriptor> renderPass;

    // One texture and uniform buffer per draw in a frame: a clip's source
    // cannot be overwritten while the draw that uses it is still queued.
    std::vector<std::unique_ptr<QRhiTexture>> sourceTextures;
    std::vector<std::unique_ptr<QRhiBuffer>> uniformBuffers;
    std::vector<std::unique_ptr<QRhiShaderResourceBindings>> bindings;

    /// A draw recorded during the frame, replayed inside the single render
    /// pass that endFrame opens.
    struct PendingDraw {
        QRhiGraphicsPipeline* pipeline{nullptr};
        QRhiShaderResourceBindings* bindings{nullptr};
    };
    std::vector<PendingDraw> draws;

    QRhiCommandBuffer* commandBuffer{nullptr};
    QSize size;
    bool inFrame{false};
};

GpuCompositor::GpuCompositor() = default;
GpuCompositor::~GpuCompositor() {
    if (state_ && state_->inFrame && state_->rhi) {
        state_->rhi->endOffscreenFrame();
    }
}

Result<std::unique_ptr<GpuCompositor>> GpuCompositor::create() {
    auto compositor = std::unique_ptr<GpuCompositor>(new GpuCompositor());
    compositor->state_ = std::make_unique<State>();
    State& state = *compositor->state_;

#if defined(Q_OS_MACOS)
    QRhiMetalInitParams params;
    state.rhi.reset(QRhi::create(QRhi::Metal, &params));
#elif defined(Q_OS_WIN)
    QRhiD3D11InitParams params;
    state.rhi.reset(QRhi::create(QRhi::D3D11, &params));
#else
    QRhiVulkanInitParams params;
    state.rhi.reset(QRhi::create(QRhi::Vulkan, &params));
#endif
    if (!state.rhi) {
        return Error{ErrorCode::Unsupported, "no GPU backend is available"};
    }

    state.vertexShader = loadShader(":/zaro/shaders/composite.vert.qsb");
    state.fragmentShader = loadShader(":/zaro/shaders/composite.frag.qsb");
    state.yuvFragmentShader = loadShader(":/zaro/shaders/composite_yuv.frag.qsb");
    if (!state.vertexShader.isValid() || !state.fragmentShader.isValid() ||
        !state.yuvFragmentShader.isValid()) {
        return Error{ErrorCode::Internal, "the compositing shaders are missing from the build"};
    }

    state.vertexBuffer.reset(
        state.rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuad)));
    if (!state.vertexBuffer->create()) {
        return Error{ErrorCode::Internal, "cannot allocate a vertex buffer"};
    }

    // Linear filtering, to match the CPU reference's bilinear sampling.
    state.sampler.reset(state.rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                              QRhiSampler::None, QRhiSampler::ClampToEdge,
                                              QRhiSampler::ClampToEdge));
    if (!state.sampler->create()) {
        return Error{ErrorCode::Internal, "cannot create a sampler"};
    }

    // Nearest for chroma, deliberately. Linear would interpolate the subsampled
    // planes, which looks better, but the CPU reference takes the nearest
    // chroma sample and the whole point of that reference is that the two agree.
    // Proper chroma siting and interpolation is a real improvement and should
    // change both paths together, not drift them apart.
    state.chromaSampler.reset(state.rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                                    QRhiSampler::None, QRhiSampler::ClampToEdge,
                                                    QRhiSampler::ClampToEdge));
    if (!state.chromaSampler->create()) {
        return Error{ErrorCode::Internal, "cannot create a chroma sampler"};
    }
    return compositor;
}

std::string GpuCompositor::backendName() const {
    return state_->rhi ? state_->rhi->backendName() : "none";
}

Status GpuCompositor::beginFrame(std::int32_t width, std::int32_t height) {
    State& state = *state_;
    if (state.inFrame) {
        return Error{ErrorCode::Internal, "a frame is already in progress"};
    }
    if (width <= 0 || height <= 0) {
        return Error{ErrorCode::InvalidData, "the output has no size"};
    }

    const QSize wanted(width, height);
    if (!state.target || state.size != wanted) {
        state.size = wanted;
        state.target.reset(
            state.rhi->newTexture(QRhiTexture::RGBA32F, wanted, 1,
                                  QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!state.target->create()) {
            return Error{ErrorCode::Unsupported, "this GPU cannot render to a 32-bit float target"};
        }
        QRhiColorAttachment attachment(state.target.get());
        state.renderTarget.reset(
            state.rhi->newTextureRenderTarget(QRhiTextureRenderTargetDescription(attachment)));
        state.renderPass.reset(state.renderTarget->newCompatibleRenderPassDescriptor());
        state.renderTarget->setRenderPassDescriptor(state.renderPass.get());
        if (!state.renderTarget->create()) {
            return Error{ErrorCode::Internal, "cannot create a render target"};
        }
        state.pipelines = {};
        state.bindingLayouts = {};
        state.yuvPipelines = {};
        state.yuvBindingLayouts = {};
    }

    if (state.rhi->beginOffscreenFrame(&state.commandBuffer) != QRhi::FrameOpSuccess) {
        return Error{ErrorCode::Internal, "cannot begin a GPU frame"};
    }
    state.inFrame = true;
    state.sourceTextures.clear();
    state.uniformBuffers.clear();
    state.bindings.clear();
    state.draws.clear();
    state.intermediateIndex = 0;

    // Uploads happen outside the render pass, which is also why there is only
    // one pass: beginPass clears the target, so opening a pass per draw would
    // wipe out everything already composited. Every clip in a frame is drawn
    // inside the single pass that endFrame opens.
    QRhiResourceUpdateBatch* batch = state.rhi->nextResourceUpdateBatch();
    batch->uploadStaticBuffer(state.vertexBuffer.get(), kQuad.data());
    state.commandBuffer->resourceUpdate(batch);
    return {};
}

Status GpuCompositor::ensureCompositePipeline(std::size_t blendIndex) {
    State& state = *state_;
    if (state.pipelines[blendIndex]) {
        return {};
    }

    auto layout =
        std::unique_ptr<QRhiShaderResourceBindings>(state.rhi->newShaderResourceBindings());
    layout->setBindings({
        // Null resources: this set exists only to describe the layout the
        // pipeline is built against. The real buffers are bound per draw.
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            nullptr),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                  nullptr, nullptr),
    });
    if (!layout->create()) {
        return Error{ErrorCode::Internal, "cannot create a resource binding layout"};
    }

    auto pipeline = std::unique_ptr<QRhiGraphicsPipeline>(state.rhi->newGraphicsPipeline());
    QRhiGraphicsPipeline::TargetBlend target;
    configureBlend(target, static_cast<BlendMode>(blendIndex));
    pipeline->setTargetBlends({target});
    pipeline->setShaderStages({{QRhiShaderStage::Vertex, state.vertexShader},
                               {QRhiShaderStage::Fragment, state.fragmentShader}});

    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({{2 * sizeof(float)}});
    inputLayout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
    pipeline->setVertexInputLayout(inputLayout);
    pipeline->setShaderResourceBindings(layout.get());
    pipeline->setRenderPassDescriptor(state.renderPass.get());
    if (!pipeline->create()) {
        return Error{ErrorCode::Internal, "cannot create a graphics pipeline"};
    }
    state.bindingLayouts[blendIndex] = std::move(layout);
    state.pipelines[blendIndex] = std::move(pipeline);
    return {};
}

Status GpuCompositor::draw(const render::RgbaImage& source, const model::Transform& transform,
                           BlendMode blend) {
    State& state = *state_;
    if (!state.inFrame) {
        return Error{ErrorCode::Internal, "draw outside a frame"};
    }
    if (!source.isValid()) {
        return Error{ErrorCode::InvalidData, "cannot draw an invalid image"};
    }
    if (transform.opacity <= 0.0 || transform.scaleX == 0.0 || transform.scaleY == 0.0) {
        return {};
    }

    const auto blendIndex = static_cast<std::size_t>(blend);
    if (Status ready = ensureCompositePipeline(blendIndex); !ready) {
        return ready;
    }

    // Upload the source. RGBA32F straight from the working space, so nothing is
    // quantised on the way to the GPU.
    auto texture = std::unique_ptr<QRhiTexture>(
        state.rhi->newTexture(QRhiTexture::RGBA32F, QSize(source.width(), source.height()), 1,
                              QRhiTexture::UsedAsTransferSource));
    if (!texture->create()) {
        return Error{ErrorCode::Internal, "cannot allocate a source texture"};
    }

    QImage staging(reinterpret_cast<const uchar*>(source.row(0)), source.width(), source.height(),
                   source.width() * static_cast<int>(sizeof(render::Rgba)),
                   QImage::Format_RGBA32FPx4);
    QRhiTextureSubresourceUploadDescription upload(staging.copy());
    QRhiTextureUploadDescription description({0, 0, upload});

    // Forward transform, mirroring the inverse the CPU applies per pixel:
    // unit quad -> source pixels -> anchor -> scale -> rotate -> position ->
    // clip space.
    QMatrix4x4 matrix;
    matrix.ortho(-0.5F * static_cast<float>(state.size.width()),
                 0.5F * static_cast<float>(state.size.width()),
                 0.5F * static_cast<float>(state.size.height()),
                 -0.5F * static_cast<float>(state.size.height()), -1.0F, 1.0F);
    matrix.translate(static_cast<float>(transform.positionX),
                     static_cast<float>(transform.positionY));
    matrix.rotate(static_cast<float>(transform.rotationDegrees), 0.0F, 0.0F, 1.0F);
    matrix.scale(static_cast<float>(transform.scaleX), static_cast<float>(transform.scaleY));
    matrix.translate(static_cast<float>(-transform.anchorX),
                     static_cast<float>(-transform.anchorY));
    matrix.scale(0.5F * static_cast<float>(source.width()),
                 0.5F * static_cast<float>(source.height()));

    auto uniforms = std::unique_ptr<QRhiBuffer>(
        state.rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformBytes));
    if (!uniforms->create()) {
        return Error{ErrorCode::Internal, "cannot allocate a uniform buffer"};
    }

    auto bindings =
        std::unique_ptr<QRhiShaderResourceBindings>(state.rhi->newShaderResourceBindings());
    bindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            uniforms.get(), 0, static_cast<quint32>(kUniformBytes)),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                  texture.get(), state.sampler.get()),
    });
    if (!bindings->create()) {
        return Error{ErrorCode::Internal, "cannot create resource bindings"};
    }

    QRhiResourceUpdateBatch* batch = state.rhi->nextResourceUpdateBatch();
    batch->uploadTexture(texture.get(), description);

    std::array<float, 20> uniformData{};
    const float* matrixData = matrix.constData();
    for (int i = 0; i < 16; ++i) {
        uniformData[static_cast<std::size_t>(i)] = matrixData[i];
    }
    uniformData[16] = static_cast<float>(transform.opacity);
    batch->updateDynamicBuffer(uniforms.get(), 0, kUniformBytes, uniformData.data());
    state.commandBuffer->resourceUpdate(batch);

    state.draws.push_back(State::PendingDraw{state.pipelines[blendIndex].get(), bindings.get()});

    // Keep everything alive until the frame is submitted.
    state.sourceTextures.push_back(std::move(texture));
    state.uniformBuffers.push_back(std::move(uniforms));
    state.bindings.push_back(std::move(bindings));
    return {};
}

/// One pass for the whole frame. Transparent black to start with, not opaque
/// black: a frame with nothing on it is empty, and the difference matters as
/// soon as anything is exported with an alpha channel.
void GpuCompositor::submitPass() {
    State& state = *state_;
    QRhiCommandBuffer* cb = state.commandBuffer;
    cb->beginPass(state.renderTarget.get(), QColor::fromRgbF(0, 0, 0, 0), {1.0F, 0});
    const QRhiCommandBuffer::VertexInput vertexInput(state.vertexBuffer.get(), 0);
    for (const State::PendingDraw& pending : state.draws) {
        cb->setGraphicsPipeline(pending.pipeline);
        cb->setViewport({0, 0, static_cast<float>(state.size.width()),
                         static_cast<float>(state.size.height())});
        cb->setShaderResources(pending.bindings);
        cb->setVertexInput(0, 1, &vertexInput);
        cb->draw(6);
    }
    cb->endPass();
}

Status GpuCompositor::endFrameOnGpu() {
    State& state = *state_;
    if (!state.inFrame) {
        return Error{ErrorCode::Internal, "endFrame without beginFrame"};
    }
    submitPass();
    state.rhi->endOffscreenFrame();
    state.inFrame = false;
    return {};
}

Status GpuCompositor::endFrame(render::RgbaImage& out) {
    State& state = *state_;
    if (!state.inFrame) {
        return Error{ErrorCode::Internal, "endFrame without beginFrame"};
    }

    QRhiCommandBuffer* cb = state.commandBuffer;

    submitPass();

    QRhiReadbackResult readback;
    QRhiResourceUpdateBatch* batch = state.rhi->nextResourceUpdateBatch();
    batch->readBackTexture(QRhiReadbackDescription(state.target.get()), &readback);
    cb->resourceUpdate(batch);
    state.rhi->endOffscreenFrame();
    state.inFrame = false;

    const int width = state.size.width();
    const int height = state.size.height();
    const auto expected =
        static_cast<qsizetype>(width) * height * static_cast<qsizetype>(sizeof(render::Rgba));
    if (readback.data.size() < expected) {
        return Error{ErrorCode::Internal, "the GPU returned less data than the frame needs"};
    }
    if (out.width() != width || out.height() != height) {
        out = render::RgbaImage{width, height};
    }
    std::memcpy(out.row(0), readback.data.constData(), static_cast<std::size_t>(expected));
    return {};
}

namespace {

/// How the working-space conversion is parameterised for the shader, mirroring
/// exactly what ColorPipeline.cpp computes on the CPU.
struct YuvParameters {
    float sampleScale{255.0F};  ///< normalised texture sample -> raw code value
    float lumaOffset{16.0F};
    float lumaScale{1.0F / 219.0F};
    float chromaScale{1.0F / 224.0F};
    float midpoint{128.0F};
    float transferId{0.0F};
    float semiPlanar{0.0F};
    float crToR{0.0F};
    float crToG{0.0F};
    float cbToG{0.0F};
    float cbToB{0.0F};
};

int transferIdFor(media::TransferFunction transfer) {
    switch (transfer) {
        case media::TransferFunction::Linear:
            return 1;
        case media::TransferFunction::SRGB:
            return 2;
        case media::TransferFunction::Gamma22:
            return 3;
        case media::TransferFunction::Gamma28:
            return 4;
        default:
            return 0;  // BT.709 / SMPTE 170M
    }
}

YuvParameters parametersFor(const media::VideoFrame& source) {
    const media::PixelFormatInfo& format = media::info(source.format());
    const int depth = format.bitsPerComponent;

    YuvParameters out;
    // Textures are R8 or R16, so a sample comes back normalised. Multiplying by
    // the container's maximum recovers the raw code value the CPU works with.
    out.sampleScale = depth > 8 ? 65535.0F : 255.0F;
    if (source.format() == media::PixelFormat::P010) {
        // P010 left-justifies its ten bits in a sixteen-bit word.
        out.sampleScale /= 64.0F;
    }

    const float peak = static_cast<float>((1 << depth) - 1);
    if (source.color().range == media::ColorRange::Full) {
        out.lumaOffset = 0.0F;
        out.lumaScale = 1.0F / peak;
        out.chromaScale = 1.0F / peak;
    } else {
        const float scale = static_cast<float>(1 << (depth - 8));
        out.lumaOffset = 16.0F * scale;
        out.lumaScale = 1.0F / (219.0F * scale);
        out.chromaScale = 1.0F / (224.0F * scale);
    }
    out.midpoint = static_cast<float>(1 << (depth - 1));
    out.transferId = static_cast<float>(transferIdFor(source.color().transfer));
    out.semiPlanar = format.planeCount == 2 ? 1.0F : 0.0F;

    float kr = 0.2126F;
    float kb = 0.0722F;
    switch (source.color().matrix) {
        case media::ColorMatrix::BT601:
            kr = 0.299F;
            kb = 0.114F;
            break;
        case media::ColorMatrix::BT2020NCL:
            kr = 0.2627F;
            kb = 0.0593F;
            break;
        case media::ColorMatrix::SMPTE240M:
            kr = 0.212F;
            kb = 0.087F;
            break;
        default:
            break;
    }
    const float kg = 1.0F - kr - kb;
    out.crToR = 2.0F * (1.0F - kr);
    out.cbToB = 2.0F * (1.0F - kb);
    out.crToG = out.crToR * kr / kg;
    out.cbToG = out.cbToB * kb / kg;
    return out;
}

}  // namespace

Status GpuCompositor::drawSource(const media::VideoFrame& source, const model::Transform& transform,
                                 BlendMode blend) {
    State& state = *state_;
    if (!state.inFrame) {
        return Error{ErrorCode::Internal, "draw outside a frame"};
    }
    if (!source.isValid()) {
        return Error{ErrorCode::InvalidData, "cannot draw an invalid frame"};
    }
    const media::PixelFormatInfo& format = media::info(source.format());
    if (!format.isPlanarYuv) {
        return Error{ErrorCode::Unsupported, std::string{"pixel format "} +
                                                 media::toString(source.format()) +
                                                 " is not a planar or semi-planar Y'CbCr layout"};
    }
    if (!render::isSupported(source.color())) {
        return Error{ErrorCode::Unsupported, std::string{"HDR transfer function '"} +
                                                 media::toString(source.color().transfer) +
                                                 "' is not handled yet"};
    }
    if (transform.opacity <= 0.0 || transform.scaleX == 0.0 || transform.scaleY == 0.0) {
        return {};
    }

    const bool deep = format.bitsPerComponent > 8;
    const bool semiPlanar = format.planeCount == 2;
    const QSize sourceSize(source.width(), source.height());

    // --- The staging surface for this draw ---------------------------------
    if (state.intermediateIndex >= state.intermediates.size()) {
        state.intermediates.emplace_back();
    }
    State::Intermediate& staging = state.intermediates[state.intermediateIndex++];
    if (!staging.texture || staging.size != sourceSize) {
        staging.size = sourceSize;
        staging.texture.reset(
            state.rhi->newTexture(QRhiTexture::RGBA32F, sourceSize, 1, QRhiTexture::RenderTarget));
        if (!staging.texture->create()) {
            return Error{ErrorCode::Unsupported, "this GPU cannot render to a float target"};
        }
        QRhiColorAttachment attachment(staging.texture.get());
        staging.target.reset(
            state.rhi->newTextureRenderTarget(QRhiTextureRenderTargetDescription(attachment)));
        staging.pass.reset(staging.target->newCompatibleRenderPassDescriptor());
        staging.target->setRenderPassDescriptor(staging.pass.get());
        if (!staging.target->create()) {
            return Error{ErrorCode::Internal, "cannot create a staging render target"};
        }
        if (state.intermediatePass == nullptr) {
            state.intermediatePass = staging.pass.get();
        }
    }

    // --- Pipelines ---------------------------------------------------------
    const auto blendIndex = static_cast<std::size_t>(blend);
    if (!state.yuvPipelines[0]) {
        auto layout =
            std::unique_ptr<QRhiShaderResourceBindings>(state.rhi->newShaderResourceBindings());
        layout->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                nullptr),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                      nullptr, nullptr),
            QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                      nullptr, nullptr),
            QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage,
                                                      nullptr, nullptr),
        });
        if (!layout->create()) {
            return Error{ErrorCode::Internal, "cannot create a resource binding layout"};
        }

        auto pipeline = std::unique_ptr<QRhiGraphicsPipeline>(state.rhi->newGraphicsPipeline());
        // No blending: this pass produces a surface, it does not composite onto
        // one. Compositing happens in the second pass, in linear light.
        pipeline->setTargetBlends({QRhiGraphicsPipeline::TargetBlend{}});
        pipeline->setShaderStages({{QRhiShaderStage::Vertex, state.vertexShader},
                                   {QRhiShaderStage::Fragment, state.yuvFragmentShader}});
        QRhiVertexInputLayout inputLayout;
        inputLayout.setBindings({{2 * sizeof(float)}});
        inputLayout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
        pipeline->setVertexInputLayout(inputLayout);
        pipeline->setShaderResourceBindings(layout.get());
        pipeline->setRenderPassDescriptor(state.intermediatePass);
        if (!pipeline->create()) {
            return Error{ErrorCode::Internal, "cannot create the colour conversion pipeline"};
        }
        state.yuvBindingLayouts[0] = std::move(layout);
        state.yuvPipelines[0] = std::move(pipeline);
    }
    if (Status ready = ensureCompositePipeline(blendIndex); !ready) {
        return ready;
    }

    // --- Upload the planes as the decoder produced them ---------------------
    QRhiResourceUpdateBatch* batch = state.rhi->nextResourceUpdateBatch();
    const auto uploadPlane = [&](std::size_t plane, QRhiTexture::Format textureFormat,
                                 int bytesPerTexel) -> QRhiTexture* {
        const auto index = static_cast<std::int32_t>(plane);
        const std::int32_t planeWidth =
            media::rowBytes(source.format(), source.width(), index) / bytesPerTexel;
        const std::int32_t rows = media::planeHeight(source.format(), source.height(), index);

        auto texture = std::unique_ptr<QRhiTexture>(state.rhi->newTexture(
            textureFormat, QSize(planeWidth, rows), 1, QRhiTexture::UsedAsTransferSource));
        if (!texture->create()) {
            return nullptr;
        }
        QRhiTextureSubresourceUploadDescription upload;
        upload.setData(QByteArray(reinterpret_cast<const char*>(source.plane(plane)),
                                  static_cast<qsizetype>(rows) * source.stride(plane)));
        upload.setDataStride(static_cast<quint32>(source.stride(plane)));
        batch->uploadTexture(texture.get(), QRhiTextureUploadDescription({0, 0, upload}));

        QRhiTexture* raw = texture.get();
        state.sourceTextures.push_back(std::move(texture));
        return raw;
    };

    const QRhiTexture::Format lumaFormat = deep ? QRhiTexture::R16 : QRhiTexture::R8;
    const int lumaBytes = deep ? 2 : 1;
    QRhiTexture* textureY = uploadPlane(0, lumaFormat, lumaBytes);
    QRhiTexture* textureCb = nullptr;
    QRhiTexture* textureCr = nullptr;
    if (semiPlanar) {
        textureCb = uploadPlane(1, deep ? QRhiTexture::RG16 : QRhiTexture::RG8, deep ? 4 : 2);
        textureCr = textureCb;  // unused by the shader; the binding must be filled
    } else {
        textureCb = uploadPlane(1, lumaFormat, lumaBytes);
        textureCr = uploadPlane(2, lumaFormat, lumaBytes);
    }
    if (textureY == nullptr || textureCb == nullptr || textureCr == nullptr) {
        return Error{ErrorCode::Internal, "cannot allocate a plane texture"};
    }

    // --- Pass one: Y'CbCr to linear light, at source resolution -------------
    const YuvParameters parameters = parametersFor(source);

    QMatrix4x4 identity;
    identity.ortho(-0.5F * static_cast<float>(sourceSize.width()),
                   0.5F * static_cast<float>(sourceSize.width()),
                   0.5F * static_cast<float>(sourceSize.height()),
                   -0.5F * static_cast<float>(sourceSize.height()), -1.0F, 1.0F);
    identity.scale(0.5F * static_cast<float>(sourceSize.width()),
                   0.5F * static_cast<float>(sourceSize.height()));

    auto convertUniforms = std::unique_ptr<QRhiBuffer>(
        state.rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kYuvUniformBytes));
    if (!convertUniforms->create()) {
        return Error{ErrorCode::Internal, "cannot allocate a uniform buffer"};
    }
    auto convertBindings =
        std::unique_ptr<QRhiShaderResourceBindings>(state.rhi->newShaderResourceBindings());
    convertBindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            convertUniforms.get(), 0, static_cast<quint32>(kYuvUniformBytes)),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                  textureY, state.chromaSampler.get()),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                  textureCb, state.chromaSampler.get()),
        QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage,
                                                  textureCr, state.chromaSampler.get()),
    });
    if (!convertBindings->create()) {
        return Error{ErrorCode::Internal, "cannot create conversion bindings"};
    }

    std::array<float, 28> convertData{};
    const float* identityData = identity.constData();
    for (int i = 0; i < 16; ++i) {
        convertData[static_cast<std::size_t>(i)] = identityData[i];
    }
    convertData[16] = 1.0F;  // opacity is applied when compositing, not here
    convertData[17] = parameters.sampleScale;
    convertData[18] = parameters.lumaOffset;
    convertData[19] = parameters.lumaScale;
    convertData[20] = parameters.chromaScale;
    convertData[21] = parameters.midpoint;
    convertData[22] = parameters.transferId;
    convertData[23] = parameters.semiPlanar;
    convertData[24] = parameters.crToR;
    convertData[25] = parameters.crToG;
    convertData[26] = parameters.cbToG;
    convertData[27] = parameters.cbToB;
    batch->updateDynamicBuffer(convertUniforms.get(), 0, kYuvUniformBytes, convertData.data());

    QRhiCommandBuffer* cb = state.commandBuffer;
    const QRhiCommandBuffer::VertexInput vertexInput(state.vertexBuffer.get(), 0);
    cb->beginPass(staging.target.get(), QColor::fromRgbF(0, 0, 0, 0), {1.0F, 0}, batch);
    cb->setGraphicsPipeline(state.yuvPipelines[0].get());
    cb->setViewport(
        {0, 0, static_cast<float>(sourceSize.width()), static_cast<float>(sourceSize.height())});
    cb->setShaderResources(convertBindings.get());
    cb->setVertexInput(0, 1, &vertexInput);
    cb->draw(6);
    cb->endPass();

    // --- Pass two: the transform, sampling linear light ---------------------
    //
    // This is why conversion gets its own pass rather than being folded into
    // the sampler. A bilinear filter applied to encoded Y'CbCr interpolates in
    // gamma space, which is the same error ADR-005 rejects for blending: the
    // midpoint between two values is not the value at the midpoint. Converting
    // first means the filter runs in linear light, and the result matches the
    // CPU reference instead of merely resembling it.
    QMatrix4x4 matrix;
    matrix.ortho(-0.5F * static_cast<float>(state.size.width()),
                 0.5F * static_cast<float>(state.size.width()),
                 0.5F * static_cast<float>(state.size.height()),
                 -0.5F * static_cast<float>(state.size.height()), -1.0F, 1.0F);
    matrix.translate(static_cast<float>(transform.positionX),
                     static_cast<float>(transform.positionY));
    matrix.rotate(static_cast<float>(transform.rotationDegrees), 0.0F, 0.0F, 1.0F);
    matrix.scale(static_cast<float>(transform.scaleX), static_cast<float>(transform.scaleY));
    matrix.translate(static_cast<float>(-transform.anchorX),
                     static_cast<float>(-transform.anchorY));
    matrix.scale(0.5F * static_cast<float>(sourceSize.width()),
                 0.5F * static_cast<float>(sourceSize.height()));

    auto uniforms = std::unique_ptr<QRhiBuffer>(
        state.rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformBytes));
    if (!uniforms->create()) {
        return Error{ErrorCode::Internal, "cannot allocate a uniform buffer"};
    }
    auto bindings =
        std::unique_ptr<QRhiShaderResourceBindings>(state.rhi->newShaderResourceBindings());
    bindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            uniforms.get(), 0, static_cast<quint32>(kUniformBytes)),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                  staging.texture.get(), state.sampler.get()),
    });
    if (!bindings->create()) {
        return Error{ErrorCode::Internal, "cannot create resource bindings"};
    }

    std::array<float, 20> uniformData{};
    const float* matrixData = matrix.constData();
    for (int i = 0; i < 16; ++i) {
        uniformData[static_cast<std::size_t>(i)] = matrixData[i];
    }
    uniformData[16] = static_cast<float>(transform.opacity);

    QRhiResourceUpdateBatch* compositeBatch = state.rhi->nextResourceUpdateBatch();
    compositeBatch->updateDynamicBuffer(uniforms.get(), 0, kUniformBytes, uniformData.data());
    cb->resourceUpdate(compositeBatch);

    state.draws.push_back(State::PendingDraw{state.pipelines[blendIndex].get(), bindings.get()});
    state.uniformBuffers.push_back(std::move(convertUniforms));
    state.uniformBuffers.push_back(std::move(uniforms));
    state.bindings.push_back(std::move(convertBindings));
    state.bindings.push_back(std::move(bindings));
    return {};
}

}  // namespace zaro::platform::qrhi
