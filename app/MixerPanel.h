#pragma once

#include <QWidget>
#include <map>
#include <vector>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/model/Project.h"
#include "zaro/core/render/AudioGraph.h"

class QCheckBox;
class QDoubleSpinBox;
class QLabel;

namespace zaro::app {

/// A meter bar: current level, and a peak that falls back slowly.
///
/// The hold is not decoration. A peak that vanishes the instant it passes is
/// unreadable — the thing a meter is for is catching the transient that went
/// over, and a transient is by definition brief.
class LevelMeter : public QWidget {
    Q_OBJECT

public:
    explicit LevelMeter(QWidget* parent = nullptr);

    /// A linear peak, 0 to 1 and beyond. Above 1 is over, and says so.
    void setLevel(float peak);
    void resetHold();
    [[nodiscard]] float hold() const noexcept { return hold_; }

protected:
    void paintEvent(QPaintEvent* event) override;
    [[nodiscard]] QSize sizeHint() const override;

private:
    float level_{0.0F};
    float hold_{0.0F};
    /// Ticks since the hold was last raised, so it falls at a fixed rate
    /// rather than at whatever rate the mixer happens to be updated.
    int held_{0};
};

/// The audio track mixer: one strip per audio track.
class MixerPanel : public QWidget {
    Q_OBJECT

public:
    explicit MixerPanel(QWidget* parent = nullptr);

    void setProject(model::Project* project, model::SequenceId sequence,
                    edit::CommandStack* commands);
    /// Rebuild the strips. Called when tracks are added or removed, and after
    /// an undo, which can do either.
    void refresh();
    /// New meter readings, from the last block the mixer produced.
    void setMeters(const render::AudioGraph::Meters& meters);

signals:
    void edited();

private:
    struct Strip {
        model::TrackId track;
        QLabel* name{nullptr};
        QDoubleSpinBox* gain{nullptr};
        QDoubleSpinBox* pan{nullptr};
        QCheckBox* mute{nullptr};
        QCheckBox* solo{nullptr};
        LevelMeter* meter{nullptr};
    };

    void push(const Strip& strip);

    model::Project* project_{nullptr};
    model::SequenceId sequenceId_;
    edit::CommandStack* commands_{nullptr};
    bool updating_{false};

    QWidget* strips_{nullptr};
    std::vector<Strip> strip_;
    LevelMeter* master_{nullptr};
};

}  // namespace zaro::app
