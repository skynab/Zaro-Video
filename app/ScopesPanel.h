#pragma once

#include <QWidget>
#include <array>

#include "zaro/core/render/Scopes.h"

class QLabel;
class QPushButton;

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

    /// What the readouts say, as the panel computes them. Public so a test can
    /// check the numbers without reading them off a label.
    struct Readings {
        double peakIre{0.0};
        double blackIre{0.0};
        double saturation{0.0};
    };
    [[nodiscard]] Readings readings() const;

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

    void setMode(Mode mode);
    void showReadings();

    Mode mode_{Mode::Waveform};
    render::FrameScopes scopes_;
    bool hasScopes_{false};
    /// Parade, Vector, Histogram, Waveform -- the design's order, which puts
    /// the two a colourist reaches for first at the left.
    std::array<QPushButton*, 4> tabs_{};
    std::array<QLabel*, 3> values_{};
    QWidget* tabBar_{nullptr};
    QWidget* readoutRow_{nullptr};
};

}  // namespace zaro::app
