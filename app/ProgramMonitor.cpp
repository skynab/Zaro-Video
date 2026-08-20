#include "ProgramMonitor.h"

#include <QString>

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

    if (const Status status = graph_->compositeOn(commandBuffer, *sequence_, position_); !status) {
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

void ProgramMonitor::setTextRasterizer(render::TextRasterizer* rasterizer) {
    text_ = rasterizer;
    if (graph_ != nullptr) {
        graph_->setTextRasterizer(rasterizer);
    }
}

}  // namespace zaro::app
