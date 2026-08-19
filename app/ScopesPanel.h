#pragma once

#include <QWidget>

#include "zaro/core/render/Scopes.h"

class QComboBox;

namespace zaro::app {

/// Waveform, RGB parade, histogram and vectorscope for the frame at the
/// playhead.
///
/// The measurement is held, not the frame: `render::measure` returns counts
/// rather than a picture, so the panel can be resized and redrawn without the
/// frame being composited again.
class ScopesPanel : public QWidget {
    Q_OBJECT

public:
    explicit ScopesPanel(QWidget* parent = nullptr);

    /// Which instrument is showing. Asked by whoever computes the measurement,
    /// so a frame is not measured for a scope nobody is looking at.
    enum class Mode { Waveform, Parade, Histogram, Vectorscope };
    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    [[nodiscard]] bool wantsMeasurement() const;

    /// Where the instrument is drawn, inside the panel. Excludes the chooser,
    /// so a caller measuring the trace does not measure its label.
    [[nodiscard]] QRect plotArea() const;

    void setScopes(render::FrameScopes scopes);
    void clear();

signals:
    /// The panel wants a fresh measurement: it became visible, or the
    /// instrument changed.
    void measurementNeeded();

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void paintWaveform(QPainter& painter, const QRect& area, const render::Waveform& waveform,
                       const QColor& colour) const;
    void paintHistogram(QPainter& painter, const QRect& area) const;
    void paintVectorscope(QPainter& painter, const QRect& area) const;

    Mode mode_{Mode::Waveform};
    render::FrameScopes scopes_;
    bool hasScopes_{false};
    QComboBox* chooser_{nullptr};
};

}  // namespace zaro::app
