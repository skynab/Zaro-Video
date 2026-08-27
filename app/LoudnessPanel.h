#pragma once

#include <QWidget>

#include "zaro/core/render/AudioGraph.h"

class QLabel;
class QPushButton;

namespace zaro::app {

/// What the programme measures, against what it has to deliver.
///
/// **Loudness is a delivery fact, not a mixing one.** A meter on a fader
/// answers "is this about to clip"; a broadcaster rejects a programme for the
/// integrated number, which is a property of the whole thing and can only be
/// known by measuring the whole thing. So the integrated figure here is taken
/// on demand -- it costs a full mix of the sequence -- while the bar beside it
/// follows the master meter live, which is the part that moves.
class LoudnessPanel : public QWidget {
    Q_OBJECT

public:
    explicit LoudnessPanel(QWidget* parent = nullptr);

    /// A finished measurement, from `AudioGraph::measureLoudness`.
    void setMeasurement(const render::AudioGraph::LoudnessResult& result);
    /// Nothing measured yet, or the cut changed under the last measurement.
    void clearMeasurement();
    /// The live master peak, for the moving bar.
    void setMasterPeak(float peak);

    /// What the programme is being held to. -23 LUFS is EBU R128; -14 is what
    /// the streaming platforms normalise to.
    [[nodiscard]] double target() const noexcept { return target_; }

signals:
    /// Somebody asked for a fresh measurement.
    void measureRequested();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    [[nodiscard]] QRect barRect() const;

    double target_{-23.0};
    bool measured_{false};
    render::AudioGraph::LoudnessResult result_;
    float masterPeak_{0.0F};
    QPushButton* measure_{nullptr};
};

}  // namespace zaro::app
