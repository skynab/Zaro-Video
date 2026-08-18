#include "zaro/platform/qrhi/GpuCompositor.h"

#include <QFile>
#include <QImage>
#include <array>
#include <cstring>
#include <vector>

#include <QMatrix4x4>
#include <rhi/qrhi.h>

namespace zaro::platform::qrhi {
namespace {

using model::BlendMode;

/// A unit quad as two triangles.
constexpr std::array<float, 12> kQuad{
    -1.0F, -1.0F, 1.0F, -1.0F, -1.0F, 1.0F, -1.0F, 1.0F, 1.0F, -1.0F, 1.0F, 1.0F,
};

/// 64 bytes of matrix plus a vec4, which satisfies std140 alignment.
constexpr int kUniformBytes = 64 + 16;

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

    // One pipeline per blend mode, built on first use. Pipelines are expensive
    // to create and cheap to keep.
    std::array<std::unique_ptr<QRhiGraphicsPipeline>, 4> pipelines;
    std::array<std::unique_ptr<QRhiShaderResourceBindings>, 4> bindingLayouts;

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
    if (!state.vertexShader.isValid() || !state.fragmentShader.isValid()) {
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
    }

    if (state.rhi->beginOffscreenFrame(&state.commandBuffer) != QRhi::FrameOpSuccess) {
        return Error{ErrorCode::Internal, "cannot begin a GPU frame"};
    }
    state.inFrame = true;
    state.sourceTextures.clear();
    state.uniformBuffers.clear();
    state.bindings.clear();
    state.draws.clear();

    // Uploads happen outside the render pass, which is also why there is only
    // one pass: beginPass clears the target, so opening a pass per draw would
    // wipe out everything already composited. Every clip in a frame is drawn
    // inside the single pass that endFrame opens.
    QRhiResourceUpdateBatch* batch = state.rhi->nextResourceUpdateBatch();
    batch->uploadStaticBuffer(state.vertexBuffer.get(), kQuad.data());
    state.commandBuffer->resourceUpdate(batch);
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
    if (!state.pipelines[blendIndex]) {
        auto layout =
            std::unique_ptr<QRhiShaderResourceBindings>(state.rhi->newShaderResourceBindings());
        layout->setBindings({
            // Null resources: this set exists only to describe the layout the
            // pipeline is built against. The real buffers are bound per draw.
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                nullptr),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                      nullptr, nullptr),
        });
        if (!layout->create()) {
            return Error{ErrorCode::Internal, "cannot create a resource binding layout"};
        }

        auto pipeline = std::unique_ptr<QRhiGraphicsPipeline>(state.rhi->newGraphicsPipeline());
        QRhiGraphicsPipeline::TargetBlend target;
        configureBlend(target, blend);
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

Status GpuCompositor::endFrame(render::RgbaImage& out) {
    State& state = *state_;
    if (!state.inFrame) {
        return Error{ErrorCode::Internal, "endFrame without beginFrame"};
    }

    QRhiCommandBuffer* cb = state.commandBuffer;

    // One pass for the whole frame. Transparent black to start with, not opaque
    // black: a frame with nothing on it is empty, and the difference matters as
    // soon as anything is exported with an alpha channel.
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

}  // namespace zaro::platform::qrhi
