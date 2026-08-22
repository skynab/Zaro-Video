#include "ProgramMonitor.h"

#include <QString>

#include "zaro/core/render/Compare.h"

namespace zaro::app {

ProgramMonitor::ProgramMonitor(QWidget* parent) : QRhiWidget{parent} {
    // The compositor works in linear float; asking the widget for a float
    // buffer keeps it that way to the end rather than quantising to 8 bits one
    // step before the screen.
    setColorBufferFormat(QRhiWidget::TextureFormat::RGBA16F);
}

ProgramMonitor::~ProgramMonitor() = default;

void ProgramMonitor::setSource(const model::Sequence* sequence,
                               render::SourceFrameProvider* provider) {
    // Ordering: the graph may not exist yet, so the rasteriser is remembered
    // and applied when it does.
    sequence_ = sequence;
    provider_ = provider;
    graph_.reset();
    update();
}

void ProgramMonitor::setPosition(const time::RationalTime& position) {
    if (position_ == position) {
        return;
    }
    position_ = position;
    update();
}

void ProgramMonitor::initialize(QRhiCommandBuffer* commandBuffer) {
    Q_UNUSED(commandBuffer);

    // Qt hands the device to us, and can hand us a different one later. Rebuild
    // rather than assume: every GPU resource we hold belongs to the old device.
    if (device_ != rhi()) {
        device_ = rhi();
        compositor_.reset();
        graph_.reset();
    }
    if (!compositor_) {
        auto created = platform::qrhi::GpuCompositor::adopt(*device_);
        if (!created) {
            lastError_ = QString::fromStdString(created.error().toString());
            return;
        }
        compositor_ = std::move(*created);
        lastError_.clear();
    }
    ensureGraph();
}

void ProgramMonitor::ensureGraph() {
    // Rebuilt whenever it is missing, not only at start-up. `setSource` drops
    // it -- the old one holds the old provider -- and if only `initialize`
    // could put it back, every re-open would leave the monitor frozen on
    // whatever frame it last drew. Switching to proxies is a re-open, which is
    // how this surfaced.
    if (!graph_ && compositor_ && provider_ != nullptr) {
        graph_ = std::make_unique<platform::qrhi::GpuRenderGraph>(*compositor_, *provider_);
        graph_->setTextRasterizer(text_);
        graph_->setProject(project_);
        graph_->setNestedSource(nestedSource_);
        graph_->setRenderCache(cache_);
    }
}

void ProgramMonitor::render(QRhiCommandBuffer* commandBuffer) {
    if (!compositor_) {
        return;
    }
    ensureGraph();
    if (sequence_ == nullptr || !graph_) {
        // Nothing loaded: leave the monitor black rather than showing whatever
        // was on screen before.
        return;
    }

    // Fitted to the screen the same way the export is fitted to the file. Read
    // from the sequence on every frame rather than set once, because the
    // delivery is an ordinary undoable edit and can change under us.
    compositor_->setPresentKnee(sequence_->output().highlightKnee);
    if (comparing_) {
        if (const Status status = renderComparison(commandBuffer); !status) {
            lastError_ = QString::fromStdString(status.error().toString());
            return;
        }
    } else if (const Status status = graph_->compositeOn(commandBuffer, *sequence_, position_);
               !status) {
        lastError_ = QString::fromStdString(status.error().toString());
        return;
    }
    if (const Status status = compositor_->presentInto(commandBuffer, renderTarget()); !status) {
        lastError_ = QString::fromStdString(status.error().toString());
        return;
    }
    lastError_.clear();
    ++framesRendered_;
}

QRectF ProgramMonitor::pictureRect() const {
    const auto widgetWidth = static_cast<double>(width());
    const auto widgetHeight = static_cast<double>(height());
    if (sequence_ == nullptr || sequence_->width() <= 0 || sequence_->height() <= 0 ||
        widgetWidth <= 0.0 || widgetHeight <= 0.0) {
        return QRectF{0.0, 0.0, widgetWidth, widgetHeight};
    }
    const double frameAspect =
        static_cast<double>(sequence_->width()) / static_cast<double>(sequence_->height());
    const double widgetAspect = widgetWidth / widgetHeight;
    double pictureWidth = widgetWidth;
    double pictureHeight = widgetHeight;
    if (widgetAspect > frameAspect) {
        pictureWidth = widgetHeight * frameAspect;
    } else {
        pictureHeight = widgetWidth / frameAspect;
    }
    return QRectF{(widgetWidth - pictureWidth) / 2.0, (widgetHeight - pictureHeight) / 2.0,
                  pictureWidth, pictureHeight};
}

void ProgramMonitor::setComparison(bool on, const time::RationalTime& reference,
                                   render::CompareMode mode, double split) {
    comparing_ = on;
    referenceAt_ = reference;
    compareMode_ = mode;
    split_ = split;
    update();
}

/// Composite both instants on the CPU and upload the arrangement.
///
/// The same bargain Phase 5v made for adjustment layers, for a related reason:
/// the GPU graph composites one instant into one target, and asking it for two
/// would mean a second target and a restructure of the draw loop for something
/// nobody exports. The render cache from Phase 5w means the reference frame is
/// composited once and read thereafter, so holding a still while grading costs
/// nothing.
Status ProgramMonitor::renderComparison(QRhiCommandBuffer* commandBuffer) {
    render::RenderGraph* cpu = graph_->cpuGraph();
    if (cpu == nullptr) {
        return Error{ErrorCode::Internal, "there is no CPU compositor to compare with"};
    }
    if (Status status = cpu->compositeInto(*sequence_, referenceAt_, referenceFrame_); !status) {
        return status;
    }
    if (Status status = cpu->compositeInto(*sequence_, position_, currentFrame_); !status) {
        return status;
    }
    render::compareFrames(referenceFrame_, currentFrame_, comparison_, compareMode_, split_);

    // A frame of its own, opened on the caller's command buffer -- the same
    // thing GpuRenderGraph::compositeOn does around its draws, and the reason
    // the first version of this reported "draw outside a frame".
    if (Status begun =
            compositor_->beginFrameOn(commandBuffer, sequence_->width(), sequence_->height());
        !begun) {
        return begun;
    }
    if (Status drawn = compositor_->draw(comparison_, model::Transform{}, model::BlendMode::Normal);
        !drawn) {
        return drawn;
    }
    return compositor_->endFrameOnGpu();
}

void ProgramMonitor::setNesting(const model::Project* project, render::FrameSource* source) {
    project_ = project;
    nestedSource_ = source;
    // Rebuilt on the next render, which is where the graph is made anyway.
    graph_.reset();
    update();
}

void ProgramMonitor::setRenderCache(render::RenderCache* cache) {
    cache_ = cache;
    if (graph_ != nullptr) {
        graph_->setRenderCache(cache);
    }
}

void ProgramMonitor::setTextRasterizer(render::TextRasterizer* rasterizer) {
    text_ = rasterizer;
    if (graph_ != nullptr) {
        graph_->setTextRasterizer(rasterizer);
    }
}

}  // namespace zaro::app
