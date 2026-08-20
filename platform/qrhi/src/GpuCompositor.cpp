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
// mat4 transform, vec4 params, vec4 white balance + exposure, vec4 grade,
// five for the secondary, one for the look, two for the mask, four for the key.
constexpr int kUniformBytes = 64 + 16 + 16 + 16 + (5 * 16) + 16 + 32 + (4 * 16);
constexpr std::size_t kUniformFloats = static_cast<std::size_t>(kUniformBytes) / sizeof(float);

/// Write a grade into the composite shader's uniform block.
///
/// Every path that binds that shader goes through this, so a new call site
/// cannot leave the grade fields as zeros -- which would be a black,
/// fully-desaturated picture rather than an obviously missing feature.
/// Upload a baked look cube as a 3D RGBA32F texture.
///
/// Sampled with hardware trilinear filtering, which is the same interpolation
/// render::LutTable does on the CPU -- so the two agree without either of them
/// knowing anything about the other's arithmetic.
std::unique_ptr<QRhiTexture> makeLutTexture(QRhi& rhi, QRhiResourceUpdateBatch& batch,
                                            const render::LutTable& lut) {
    constexpr int kSize = render::LutTable::kSize;
    auto texture = std::unique_ptr<QRhiTexture>(rhi.newTexture(
        QRhiTexture::RGBA32F, kSize, kSize, kSize, 1, QRhiTexture::UsedAsTransferSource));
    if (texture == nullptr || !texture->create()) {
        return nullptr;
    }

    // Padded to four components: three-component float textures are not
    // universally sampleable, and this is uploaded once per look rather than
    // per frame.
    std::vector<float> padded(static_cast<std::size_t>(kSize) * kSize * kSize * 4, 1.0F);
    const float* entries = lut.data();
    for (std::size_t i = 0; i < static_cast<std::size_t>(kSize) * kSize * kSize; ++i) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            padded[(i * 4) + channel] = entries[(i * 3) + channel];
        }
    }

    // One upload entry per z slice: a 3D texture is uploaded a layer at a time.
    QRhiTextureUploadDescription description;
    std::vector<QRhiTextureUploadEntry> slices;
    std::vector<QImage> keepAlive;
    keepAlive.reserve(static_cast<std::size_t>(kSize));
    slices.reserve(static_cast<std::size_t>(kSize));
    for (int z = 0; z < kSize; ++z) {
        const float* start = padded.data() + (static_cast<std::size_t>(z) * kSize * kSize * 4);
        QImage slice(reinterpret_cast<const uchar*>(start), kSize, kSize,
                     kSize * 4 * static_cast<int>(sizeof(float)), QImage::Format_RGBA32FPx4);
        keepAlive.push_back(slice.copy());
        slices.emplace_back(z, 0, QRhiTextureSubresourceUploadDescription(keepAlive.back()));
    }
    description.setEntries(slices.cbegin(), slices.cend());
    batch.uploadTexture(texture.get(), description);
    return texture;
}

/// Upload a baked curve table as a 1024x1 RGBA32F texture.
///
/// RGBA rather than RGB: three-component float textures are not universally
/// supported for sampling, and a quarter of 16 kB is not worth the risk of a
/// format that works on one machine and not the next.
std::unique_ptr<QRhiTexture> makeCurveTexture(QRhi& rhi, QRhiResourceUpdateBatch& batch,
                                              const render::CurveTable& table) {
    constexpr int kEntries = render::CurveTable::kEntries;
    auto texture = std::unique_ptr<QRhiTexture>(rhi.newTexture(
        QRhiTexture::RGBA32F, QSize(kEntries, 1), 1, QRhiTexture::UsedAsTransferSource));
    if (!texture->create()) {
        return nullptr;
    }

    std::vector<float> padded(static_cast<std::size_t>(kEntries) * 4, 0.0F);
    const float* entries = table.data();
    for (int i = 0; i < kEntries; ++i) {
        for (int channel = 0; channel < 3; ++channel) {
            padded[(static_cast<std::size_t>(i) * 4) + static_cast<std::size_t>(channel)] =
                entries[(static_cast<std::size_t>(i) * 3) + static_cast<std::size_t>(channel)];
        }
        padded[(static_cast<std::size_t>(i) * 4) + 3] = 1.0F;
    }

    QImage staging(reinterpret_cast<const uchar*>(padded.data()), kEntries, 1,
                   kEntries * 4 * static_cast<int>(sizeof(float)), QImage::Format_RGBA32FPx4);
    QRhiTextureSubresourceUploadDescription upload(staging.copy());
    batch.uploadTexture(texture.get(), QRhiTextureUploadDescription({0, 0, upload}));
    return texture;
}

void writeGrade(std::array<float, kUniformFloats>& uniformData, const render::GradeConstants& grade,
                bool curved, const render::SecondaryConstants* secondary) {
    uniformData[20] = grade.balance.r;
    uniformData[21] = grade.balance.g;
    uniformData[22] = grade.balance.b;
    uniformData[23] = grade.exposure;
    uniformData[24] = grade.contrast;
    uniformData[25] = grade.saturation;
    // A flag rather than an identity table: sampling one would round every
    // ungraded pixel through the table's own resolution, and an ungraded clip
    // has to come out bit-identical.
    uniformData[26] = curved ? 1.0F : 0.0F;

    // Five more vec4s: the secondary's own correction and its three windows.
    // Written here rather than at each call site for the same reason the flag
    // is -- a site that forgot them would key on zeros, which selects nothing
    // and looks exactly like a feature that is switched off.
    const bool keyed = secondary != nullptr && secondary->isActive();
    uniformData[28] = keyed ? secondary->grade.balance.r : 1.0F;
    uniformData[29] = keyed ? secondary->grade.balance.g : 1.0F;
    uniformData[30] = keyed ? secondary->grade.balance.b : 1.0F;
    uniformData[31] = keyed ? secondary->grade.exposure : 1.0F;
    uniformData[32] = keyed ? secondary->grade.contrast : 1.0F;
    uniformData[33] = keyed ? secondary->grade.saturation : 1.0F;
    uniformData[34] = keyed && secondary->showMask ? 1.0F : 0.0F;
    uniformData[35] = keyed ? 1.0F : 0.0F;
    if (!keyed) {
        return;
    }
    const render::QualifierConstants& window = secondary->qualifier;
    uniformData[36] = window.hueCentre;
    uniformData[37] = window.hueInner;
    uniformData[38] = window.hueOuter;
    uniformData[40] = window.satInnerLow;
    uniformData[41] = window.satOuterLow;
    uniformData[42] = window.satInnerHigh;
    uniformData[43] = window.satOuterHigh;
    uniformData[44] = window.lumaInnerLow;
    uniformData[45] = window.lumaOuterLow;
    uniformData[46] = window.lumaInnerHigh;
    uniformData[47] = window.lumaOuterHigh;
}

void writeKeyer(std::array<float, kUniformFloats>& uniformData,
                const render::KeyerConstants* keyer) {
    // Written unconditionally, like the secondary above and for the same
    // reason: a call site that forgot would leave a kind of zero, which keys
    // nothing and looks exactly like a feature that is switched off.
    const bool keying = keyer != nullptr && keyer->isActive();
    if (!keying) {
        uniformData[63] = 0.0F;  // no key
        return;
    }
    uniformData[60] = keyer->keyR;
    uniformData[61] = keyer->keyG;
    uniformData[62] = keyer->keyB;
    uniformData[63] = keyer->kind == model::KeyKind::Luma ? 2.0F : 1.0F;
    uniformData[64] = keyer->tolerance;
    uniformData[65] = keyer->outer;
    uniformData[66] = keyer->spill;
    uniformData[67] = static_cast<float>(keyer->spillChannel);
    uniformData[68] = keyer->lumaInnerLow;
    uniformData[69] = keyer->lumaOuterLow;
    uniformData[70] = keyer->lumaInnerHigh;
    uniformData[71] = keyer->lumaOuterHigh;
    uniformData[72] = keyer->showMatte ? 1.0F : 0.0F;
}

void writeLook(std::array<float, kUniformFloats>& uniformData, const render::LutTable* lut,
               float amount) {
    const bool looked = lut != nullptr && lut->isValid() && amount > 0.0F;
    uniformData[48] = looked ? amount : 0.0F;
    uniformData[49] = looked ? lut->axisMax() : 1.0F;
    uniformData[50] = looked ? 1.0F : 0.0F;
    // The size travels with the cube rather than being written out in the
    // shader, so the two cannot disagree about how big it is.
    uniformData[51] = static_cast<float>(render::LutTable::kSize);
}

void writeMask(std::array<float, kUniformFloats>& uniformData, const model::Mask* mask,
               QSize frame) {
    // The frame size travels with every draw: the vertex shader needs it to
    // turn a clip position back into output pixels, which is the space a mask
    // is written in.
    uniformData[18] = static_cast<float>(frame.width());
    uniformData[19] = static_cast<float>(frame.height());
    if (mask == nullptr || !mask->isSet()) {
        uniformData[54] = 0.0F;  // no shape
        return;
    }
    uniformData[52] = static_cast<float>(mask->width * 0.5);
    uniformData[53] = static_cast<float>(mask->height * 0.5);
    // Slots 54 and 55 are the centre; the shape flag lives in the next vector.
    uniformData[54] = static_cast<float>(mask->centreX);
    uniformData[55] = static_cast<float>(mask->centreY);
    uniformData[56] = static_cast<float>(mask->cornerRadius);
    uniformData[57] = static_cast<float>(mask->feather);
    uniformData[58] = mask->shape == model::MaskShape::Ellipse ? 2.0F : 1.0F;
    uniformData[59] = mask->inverted ? 1.0F : 0.0F;
}
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
    /// Set only when this compositor created the device. When a device is
    /// adopted this stays null and `rhi` points at someone else's.
    std::unique_ptr<QRhi> ownedRhi;
    QRhi* rhi{nullptr};

    // Presenting into an external target needs its own pipeline, because a
    // pipeline is tied to the render pass it was built for.
    std::unique_ptr<QRhiGraphicsPipeline> presentPipeline;
    std::unique_ptr<QRhiShaderResourceBindings> presentBindings;
    std::unique_ptr<QRhiBuffer> presentUniforms;
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
    /// Bound wherever a curve table is not in use. The binding has to exist
    /// because the pipeline layout says it does; the shader never reads it.
    std::unique_ptr<QRhiTexture> noCurve;
    std::unique_ptr<QRhiTexture> noLut;

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
    /// False when recording into a command buffer someone else owns.
    bool ownsFrame{true};
};

GpuCompositor::GpuCompositor() = default;
GpuCompositor::~GpuCompositor() {
    if (state_ && state_->inFrame && state_->rhi) {
        state_->rhi->endOffscreenFrame();
    }
}

namespace {

/// Everything that has to exist on whichever device the compositor ends up
/// using. Shared between creating a device and adopting one.
Status buildDeviceResources(GpuCompositor::State& state);

}  // namespace

Result<std::unique_ptr<GpuCompositor>> GpuCompositor::create() {
    auto compositor = std::unique_ptr<GpuCompositor>(new GpuCompositor());
    compositor->state_ = std::make_unique<State>();
    State& state = *compositor->state_;

#if defined(Q_OS_MACOS)
    QRhiMetalInitParams params;
    state.ownedRhi.reset(QRhi::create(QRhi::Metal, &params));
#elif defined(Q_OS_WIN)
    QRhiD3D11InitParams params;
    state.ownedRhi.reset(QRhi::create(QRhi::D3D11, &params));
#else
    QRhiVulkanInitParams params;
    state.ownedRhi.reset(QRhi::create(QRhi::Vulkan, &params));
#endif
    if (!state.ownedRhi) {
        return Error{ErrorCode::Unsupported, "no GPU backend is available"};
    }
    state.rhi = state.ownedRhi.get();

    if (Status status = buildDeviceResources(state); !status) {
        return status.error();
    }
    return compositor;
}

Result<std::unique_ptr<GpuCompositor>> GpuCompositor::adopt(::QRhi& device) {
    auto compositor = std::unique_ptr<GpuCompositor>(new GpuCompositor());
    compositor->state_ = std::make_unique<State>();
    State& state = *compositor->state_;

    // Borrowed, not owned. The widget that handed us this device outlives us
    // and will destroy it itself.
    state.rhi = reinterpret_cast<QRhi*>(&device);

    if (Status status = buildDeviceResources(state); !status) {
        return status.error();
    }
    return compositor;
}

namespace {

Status buildDeviceResources(GpuCompositor::State& state) {
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
    state.noCurve.reset(state.rhi->newTexture(QRhiTexture::RGBA32F, QSize(1, 1), 1,
                                              QRhiTexture::UsedAsTransferSource));
    if (!state.noCurve->create()) {
        return Error{ErrorCode::Internal, "cannot allocate the placeholder curve texture"};
    }

    state.noLut.reset(
        state.rhi->newTexture(QRhiTexture::RGBA32F, 1, 1, 1, 1, QRhiTexture::UsedAsTransferSource));
    if (state.noLut == nullptr || !state.noLut->create()) {
        return Error{ErrorCode::Internal, "cannot allocate the placeholder look texture"};
    }

    state.chromaSampler.reset(state.rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                                    QRhiSampler::None, QRhiSampler::ClampToEdge,
                                                    QRhiSampler::ClampToEdge));
    if (!state.chromaSampler->create()) {
        return Error{ErrorCode::Internal, "cannot create a chroma sampler"};
    }
    return {};
}

}  // namespace

std::string GpuCompositor::backendName() const {
    return state_->rhi ? state_->rhi->backendName() : "none";
}

Status GpuCompositor::ensureTarget(std::int32_t width, std::int32_t height) {
    State& state = *state_;
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
        state.presentPipeline.reset();
        state.presentBindings.reset();
    }
    return {};
}

void GpuCompositor::startRecording() {
    State& state = *state_;
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
}

Status GpuCompositor::beginFrame(std::int32_t width, std::int32_t height) {
    State& state = *state_;
    if (state.inFrame) {
        return Error{ErrorCode::Internal, "a frame is already in progress"};
    }
    if (Status status = ensureTarget(width, height); !status) {
        return status;
    }
    if (state.rhi->beginOffscreenFrame(&state.commandBuffer) != QRhi::FrameOpSuccess) {
        return Error{ErrorCode::Internal, "cannot begin a GPU frame"};
    }
    state.ownsFrame = true;
    startRecording();
    return {};
}

Status GpuCompositor::beginFrameOn(::QRhiCommandBuffer* commandBuffer, std::int32_t width,
                                   std::int32_t height) {
    State& state = *state_;
    if (state.inFrame) {
        return Error{ErrorCode::Internal, "a frame is already in progress"};
    }
    if (commandBuffer == nullptr) {
        return Error{ErrorCode::InvalidData, "beginFrameOn needs a command buffer"};
    }
    if (Status status = ensureTarget(width, height); !status) {
        return status;
    }
    // Record into the caller's frame. A widget has already opened one by the
    // time it asks us to draw, and opening a second would be an error rather
    // than a nesting.
    state.commandBuffer = reinterpret_cast<QRhiCommandBuffer*>(commandBuffer);
    state.ownsFrame = false;
    startRecording();
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
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                  nullptr, nullptr),
        QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage,
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
                           BlendMode blend, const render::GradeConstants& grade,
                           const render::CurveTable* curves,
                           const render::SecondaryConstants* secondary, const render::LutTable* lut,
                           float lutAmount, const model::Mask* mask,
                           const render::KeyerConstants* keyer) {
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

    QRhiResourceUpdateBatch* batch = state.rhi->nextResourceUpdateBatch();
    batch->uploadTexture(texture.get(), description);

    const bool curved = curves != nullptr && !curves->isIdentity();
    const bool looked = lut != nullptr && lut->isValid() && lutAmount > 0.0F;
    std::unique_ptr<QRhiTexture> lutTexture;
    std::unique_ptr<QRhiTexture> curveTexture;
    if (curved) {
        curveTexture = makeCurveTexture(*state.rhi, *batch, *curves);
        if (curveTexture == nullptr) {
            return Error{ErrorCode::Internal, "cannot allocate a curve texture"};
        }
    }
    if (looked) {
        lutTexture = makeLutTexture(*state.rhi, *batch, *lut);
        if (lutTexture == nullptr) {
            return Error{ErrorCode::Internal, "cannot allocate a look texture"};
        }
    }

    auto bindings =
        std::unique_ptr<QRhiShaderResourceBindings>(state.rhi->newShaderResourceBindings());
    bindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            uniforms.get(), 0, static_cast<quint32>(kUniformBytes)),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                  texture.get(), state.sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                  curved ? curveTexture.get() : state.noCurve.get(),
                                                  state.sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage,
                                                  looked ? lutTexture.get() : state.noLut.get(),
                                                  state.sampler.get()),
    });
    if (!bindings->create()) {
        return Error{ErrorCode::Internal, "cannot create resource bindings"};
    }

    std::array<float, kUniformFloats> uniformData{};
    const float* matrixData = matrix.constData();
    for (int i = 0; i < 16; ++i) {
        uniformData[static_cast<std::size_t>(i)] = matrixData[i];
    }
    uniformData[16] = static_cast<float>(transform.opacity);
    writeGrade(uniformData, grade, curved, secondary);
    writeLook(uniformData, lut, lutAmount);
    writeMask(uniformData, mask, state.size);
    writeKeyer(uniformData, keyer);
    batch->updateDynamicBuffer(uniforms.get(), 0, kUniformBytes, uniformData.data());
    state.commandBuffer->resourceUpdate(batch);

    state.draws.push_back(State::PendingDraw{state.pipelines[blendIndex].get(), bindings.get()});

    // Keep everything alive until the frame is submitted. The bindings hold
    // raw pointers to all of it, so anything dropped here is read after it is
    // freed -- which is a segfault, not a wrong picture.
    state.sourceTextures.push_back(std::move(texture));
    if (curveTexture != nullptr) {
        state.sourceTextures.push_back(std::move(curveTexture));
    }
    if (lutTexture != nullptr) {
        state.sourceTextures.push_back(std::move(lutTexture));
    }
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
    // Only close the frame if we opened it. When recording into someone else's
    // command buffer, ending their frame would submit it out from under them.
    if (state.ownsFrame) {
        state.rhi->endOffscreenFrame();
    }
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
                                 const render::GradeConstants& grade, BlendMode blend,
                                 const render::CurveTable* curves,
                                 const render::SecondaryConstants* secondary,
                                 const render::LutTable* lut, float lutAmount,
                                 const model::Mask* mask, const render::KeyerConstants* keyer) {
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
    const bool curved = curves != nullptr && !curves->isIdentity();
    const bool looked = lut != nullptr && lut->isValid() && lutAmount > 0.0F;
    std::unique_ptr<QRhiTexture> lutTexture;
    std::unique_ptr<QRhiTexture> curveTexture;
    if (curved) {
        QRhiResourceUpdateBatch* curveBatch = state.rhi->nextResourceUpdateBatch();
        curveTexture = makeCurveTexture(*state.rhi, *curveBatch, *curves);
        if (curveTexture == nullptr) {
            return Error{ErrorCode::Internal, "cannot allocate a curve texture"};
        }
        state.commandBuffer->resourceUpdate(curveBatch);
    }
    if (looked) {
        QRhiResourceUpdateBatch* lutBatch = state.rhi->nextResourceUpdateBatch();
        lutTexture = makeLutTexture(*state.rhi, *lutBatch, *lut);
        if (lutTexture == nullptr) {
            return Error{ErrorCode::Internal, "cannot allocate a look texture"};
        }
        state.commandBuffer->resourceUpdate(lutBatch);
    }

    auto bindings =
        std::unique_ptr<QRhiShaderResourceBindings>(state.rhi->newShaderResourceBindings());
    bindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            uniforms.get(), 0, static_cast<quint32>(kUniformBytes)),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                  staging.texture.get(), state.sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                  curved ? curveTexture.get() : state.noCurve.get(),
                                                  state.sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage,
                                                  looked ? lutTexture.get() : state.noLut.get(),
                                                  state.sampler.get()),
    });
    if (!bindings->create()) {
        return Error{ErrorCode::Internal, "cannot create resource bindings"};
    }

    std::array<float, kUniformFloats> uniformData{};
    const float* matrixData = matrix.constData();
    for (int i = 0; i < 16; ++i) {
        uniformData[static_cast<std::size_t>(i)] = matrixData[i];
    }
    uniformData[16] = static_cast<float>(transform.opacity);
    writeGrade(uniformData, grade, curved, secondary);
    writeLook(uniformData, lut, lutAmount);
    writeMask(uniformData, mask, state.size);
    writeKeyer(uniformData, keyer);

    QRhiResourceUpdateBatch* compositeBatch = state.rhi->nextResourceUpdateBatch();
    compositeBatch->updateDynamicBuffer(uniforms.get(), 0, kUniformBytes, uniformData.data());
    cb->resourceUpdate(compositeBatch);

    state.draws.push_back(State::PendingDraw{state.pipelines[blendIndex].get(), bindings.get()});
    if (curveTexture != nullptr) {
        state.sourceTextures.push_back(std::move(curveTexture));
    }
    if (lutTexture != nullptr) {
        state.sourceTextures.push_back(std::move(lutTexture));
    }
    state.uniformBuffers.push_back(std::move(convertUniforms));
    state.uniformBuffers.push_back(std::move(uniforms));
    state.bindings.push_back(std::move(convertBindings));
    state.bindings.push_back(std::move(bindings));
    return {};
}

Status GpuCompositor::presentInto(::QRhiCommandBuffer* commandBuffer, ::QRhiRenderTarget* target) {
    State& state = *state_;
    auto* cb = reinterpret_cast<QRhiCommandBuffer*>(commandBuffer);
    auto* rt = reinterpret_cast<QRhiRenderTarget*>(target);

    if (cb == nullptr || rt == nullptr) {
        return Error{ErrorCode::InvalidData, "presentInto needs a command buffer and a target"};
    }
    if (!state.target) {
        return Error{ErrorCode::Internal, "there is no composited frame to present"};
    }
    if (state.inFrame) {
        return Error{ErrorCode::Internal, "presentInto while a frame is still open"};
    }

    // Built once, against the target's render pass. A pipeline is tied to the
    // pass it was created for, so this cannot reuse the offscreen one.
    if (!state.presentPipeline) {
        state.presentUniforms.reset(
            state.rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformBytes));
        if (!state.presentUniforms->create()) {
            return Error{ErrorCode::Internal, "cannot allocate the present uniform buffer"};
        }

        state.presentBindings.reset(state.rhi->newShaderResourceBindings());
        state.presentBindings->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                state.presentUniforms.get(), 0, static_cast<quint32>(kUniformBytes)),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                      state.target.get(), state.sampler.get()),
            // Never read: the present pass has no grade. The binding exists
            // because the pipeline layout says it does.
            QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                      state.noCurve.get(), state.sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage,
                                                      state.noLut.get(), state.sampler.get()),
        });
        if (!state.presentBindings->create()) {
            return Error{ErrorCode::Internal, "cannot create present bindings"};
        }

        state.presentPipeline.reset(state.rhi->newGraphicsPipeline());
        QRhiGraphicsPipeline::TargetBlend blend;
        // The composited frame is premultiplied and goes onto an opaque
        // backdrop, so `over` is right here too.
        blend.enable = true;
        blend.srcColor = QRhiGraphicsPipeline::One;
        blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        blend.srcAlpha = QRhiGraphicsPipeline::One;
        blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        state.presentPipeline->setTargetBlends({blend});
        state.presentPipeline->setShaderStages({{QRhiShaderStage::Vertex, state.vertexShader},
                                                {QRhiShaderStage::Fragment, state.fragmentShader}});

        QRhiVertexInputLayout inputLayout;
        inputLayout.setBindings({{2 * sizeof(float)}});
        inputLayout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
        state.presentPipeline->setVertexInputLayout(inputLayout);
        state.presentPipeline->setShaderResourceBindings(state.presentBindings.get());
        state.presentPipeline->setRenderPassDescriptor(rt->renderPassDescriptor());
        if (!state.presentPipeline->create()) {
            return Error{ErrorCode::Internal, "cannot create the present pipeline"};
        }
    }

    // Letterbox: fit the frame inside the target without distorting it. A
    // preview that quietly stretches the picture is worse than useless, because
    // every framing decision made against it is wrong.
    const QSize targetSize = rt->pixelSize();
    const float frameAspect =
        static_cast<float>(state.size.width()) / static_cast<float>(state.size.height());
    const float targetAspect =
        static_cast<float>(targetSize.width()) / static_cast<float>(targetSize.height());
    float scaleX = 1.0F;
    float scaleY = 1.0F;
    if (frameAspect > targetAspect) {
        scaleY = targetAspect / frameAspect;
    } else {
        scaleX = frameAspect / targetAspect;
    }

    QMatrix4x4 matrix;
    // Flip vertically when the backend's framebuffer origin is at the top.
    //
    // The composited texture is written with row 0 as the top of the picture.
    // The present quad maps texture V=0 to the bottom of clip space, so on a
    // Y-down backend -- Metal, Vulkan, D3D -- the picture arrives upside down,
    // while on a Y-up one -- OpenGL -- it does not. Getting this wrong is
    // invisible to any numeric check that only asks whether pixels were lit.
    const float flip = state.rhi->isYUpInFramebuffer() ? 1.0F : -1.0F;
    matrix.scale(scaleX, scaleY * flip);

    QRhiResourceUpdateBatch* batch = state.rhi->nextResourceUpdateBatch();
    std::array<float, kUniformFloats> uniformData{};
    const float* matrixData = matrix.constData();
    for (int i = 0; i < 16; ++i) {
        uniformData[static_cast<std::size_t>(i)] = matrixData[i];
    }
    uniformData[16] = 1.0F;  // opacity
    // The present pass shows what was already composited. Grading here would
    // apply every clip's correction a second time, to the whole frame.
    writeGrade(uniformData, render::GradeConstants{}, false, nullptr);
    writeLook(uniformData, nullptr, 0.0F);
    writeMask(uniformData, nullptr, state.size);
    // Nor keying: the frame being presented has already had every clip's key
    // applied to it, and a second one would cut holes in the composite.
    writeKeyer(uniformData, nullptr);
    batch->updateDynamicBuffer(state.presentUniforms.get(), 0, kUniformBytes, uniformData.data());

    // Clear to opaque black: the bars either side of a letterboxed frame are
    // part of the picture area, not a hole in the window.
    cb->beginPass(rt, QColor::fromRgbF(0, 0, 0, 1), {1.0F, 0}, batch);
    cb->setGraphicsPipeline(state.presentPipeline.get());
    cb->setViewport(
        {0, 0, static_cast<float>(targetSize.width()), static_cast<float>(targetSize.height())});
    cb->setShaderResources(state.presentBindings.get());
    const QRhiCommandBuffer::VertexInput vertexInput(state.vertexBuffer.get(), 0);
    cb->setVertexInput(0, 1, &vertexInput);
    cb->draw(6);
    cb->endPass();
    return {};
}

Status GpuCompositor::presentToImage(std::int32_t width, std::int32_t height,
                                     render::RgbaImage& out) {
    State& state = *state_;
    if (!state.target) {
        return Error{ErrorCode::Internal, "there is no composited frame to present"};
    }
    if (width <= 0 || height <= 0) {
        return Error{ErrorCode::InvalidData, "the output has no size"};
    }

    std::unique_ptr<QRhiTexture> texture(
        state.rhi->newTexture(QRhiTexture::RGBA32F, QSize(width, height), 1,
                              QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!texture->create()) {
        return Error{ErrorCode::Unsupported, "cannot create a presentation target"};
    }
    QRhiColorAttachment attachment(texture.get());
    std::unique_ptr<QRhiTextureRenderTarget> target(
        state.rhi->newTextureRenderTarget(QRhiTextureRenderTargetDescription(attachment)));
    std::unique_ptr<QRhiRenderPassDescriptor> pass(target->newCompatibleRenderPassDescriptor());
    target->setRenderPassDescriptor(pass.get());
    if (!target->create()) {
        return Error{ErrorCode::Internal, "cannot create a presentation render target"};
    }

    // A different render pass from any previous one, so the pipeline built
    // against the last target cannot be reused.
    state.presentPipeline.reset();
    state.presentBindings.reset();

    QRhiCommandBuffer* cb = nullptr;
    if (state.rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) {
        return Error{ErrorCode::Internal, "cannot begin a GPU frame"};
    }
    if (Status status = presentInto(reinterpret_cast<::QRhiCommandBuffer*>(cb),
                                    reinterpret_cast<::QRhiRenderTarget*>(target.get()));
        !status) {
        state.rhi->endOffscreenFrame();
        return status;
    }

    QRhiReadbackResult readback;
    QRhiResourceUpdateBatch* batch = state.rhi->nextResourceUpdateBatch();
    batch->readBackTexture(QRhiReadbackDescription(texture.get()), &readback);
    cb->resourceUpdate(batch);
    state.rhi->endOffscreenFrame();

    // Built against a target that is about to be destroyed.
    state.presentPipeline.reset();
    state.presentBindings.reset();

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
