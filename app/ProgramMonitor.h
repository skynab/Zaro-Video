#pragma once

#include <QRectF>
#include <QRhiWidget>
#include <memory>

#include "zaro/core/model/Sequence.h"
#include "zaro/core/render/Compare.h"
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

    /// Where the CPU fallback keeps its frames. Held until the graph exists,
    /// for the same reason the rasteriser is.
    void setRenderCache(render::RenderCache* cache);

    void setPosition(const time::RationalTime& position);

    /// Show the frame against a reference one.
    ///
    /// A viewing arrangement, not a render: nothing here reaches an export.
    /// Turning it on takes the monitor down the CPU path, the same one an
    /// adjustment layer forces -- two composites of the same sequence at
    /// different instants is not something the GPU graph is shaped for, and the
    /// render cache from Phase 5w is what makes that affordable.
    void setComparison(bool on, const time::RationalTime& reference, render::CompareMode mode,
                       double split);
    [[nodiscard]] bool comparing() const noexcept { return comparing_; }
    [[nodiscard]] const time::RationalTime& position() const noexcept { return position_; }

    /// Empty when the last frame rendered cleanly.
    [[nodiscard]] const QString& lastError() const noexcept { return lastError_; }

    [[nodiscard]] std::int64_t framesRendered() const noexcept { return framesRendered_; }

    /// Where the picture sits inside this widget, in widget pixels.
    ///
    /// The frame is letterboxed to keep its aspect ratio, so anything drawn
    /// *over* the monitor -- a mask outline, a handle somebody is dragging --
    /// has to know where the bars are. Computed here rather than by the caller
    /// because the compositor's present pass computes the same fit, and two
    /// places deciding where the picture is would put an overlay a few pixels
    /// off the thing it is supposed to be on.
    [[nodiscard]] QRectF pictureRect() const;

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
    render::RenderCache* cache_{nullptr};
    bool comparing_{false};
    time::RationalTime referenceAt_{};
    render::CompareMode compareMode_{render::CompareMode::Split};
    double split_{0.5};
    render::RgbaImage referenceFrame_;
    render::RgbaImage currentFrame_;
    render::RgbaImage comparison_;
    [[nodiscard]] Status renderComparison(QRhiCommandBuffer* commandBuffer);
    time::RationalTime position_{};

    std::unique_ptr<platform::qrhi::GpuCompositor> compositor_;
    std::unique_ptr<platform::qrhi::GpuRenderGraph> graph_;
    QRhi* device_{nullptr};
    QString lastError_;
    std::int64_t framesRendered_{0};
};

}  // namespace zaro::app
