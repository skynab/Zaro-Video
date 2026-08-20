#pragma once

#include <QRhiWidget>
#include <memory>

#include "zaro/core/model/Sequence.h"
#include "zaro/core/render/FrameSource.h"
#include "zaro/core/render/TextRasterizer.h"
#include "zaro/core/time/RationalTime.h"
#include "zaro/platform/qrhi/GpuCompositor.h"
#include "zaro/platform/qrhi/GpuRenderGraph.h"

namespace zaro::app {

/// The program monitor: what the timeline looks like at the playhead.
///
/// This is the point of the GPU path. The compositor adopts the widget's own
/// device, so the composited texture is drawn straight to the screen and never
/// touches system memory. Phase 3d measured that readback at the difference
/// between roughly 550 fps and 95.
class ProgramMonitor : public QRhiWidget {
    Q_OBJECT

public:
    explicit ProgramMonitor(QWidget* parent = nullptr);
    ~ProgramMonitor() override;

    /// Neither is owned; both must outlive the widget.
    void setSource(const model::Sequence* sequence, render::SourceFrameProvider* provider);

    /// How to draw text layers. Held until the graph exists, since the graph is
    /// built lazily when the GPU device is ready.
    void setTextRasterizer(render::TextRasterizer* rasterizer);

    /// What nested clips are resolved against, and what composites them.
    void setNesting(const model::Project* project, render::FrameSource* source);

    void setPosition(const time::RationalTime& position);
    [[nodiscard]] const time::RationalTime& position() const noexcept { return position_; }

    /// Empty when the last frame rendered cleanly.
    [[nodiscard]] const QString& lastError() const noexcept { return lastError_; }

    [[nodiscard]] std::int64_t framesRendered() const noexcept { return framesRendered_; }

protected:
    void initialize(QRhiCommandBuffer* commandBuffer) override;
    void render(QRhiCommandBuffer* commandBuffer) override;

private:
    const model::Sequence* sequence_{nullptr};
    void ensureGraph();

    render::SourceFrameProvider* provider_{nullptr};
    render::TextRasterizer* text_{nullptr};
    const model::Project* project_{nullptr};
    render::FrameSource* nestedSource_{nullptr};
    time::RationalTime position_{};

    std::unique_ptr<platform::qrhi::GpuCompositor> compositor_;
    std::unique_ptr<platform::qrhi::GpuRenderGraph> graph_;
    QRhi* device_{nullptr};
    QString lastError_;
    std::int64_t framesRendered_{0};
};

}  // namespace zaro::app
