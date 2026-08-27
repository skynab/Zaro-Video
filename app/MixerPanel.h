#pragma once

#include <QWidget>
#include <map>
#include <vector>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/model/Project.h"
#include "zaro/core/render/AudioGraph.h"

#include "AudioStrip.h"

class QLabel;
class QPushButton;

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

/// The audio track mixer: one console strip per audio track, and the master.
///
/// The strips are `AudioStrip`, which paints itself; this panel is what stands
/// between them and the project. Every move is written through the command
/// stack and then read back, so an undo puts the faders where the project says
/// they are rather than where the widget last remembered being.
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

    /// Which channel the detail panel is showing. Invalid until a strip is
    /// clicked or the first refresh picks the first track.
    [[nodiscard]] model::TrackId picked() const noexcept { return picked_; }

    /// Drop every solo. The design gives this its own button, because a solo
    /// left on in one strip is the commonest way to wonder where the sound
    /// went.
    void clearSolos();
    /// Every fader back to unity, pans back to centre.
    void resetFaders();

signals:
    void edited();
    /// A different channel was picked, so the channel panel should follow.
    void pickedChanged(zaro::model::TrackId track);

private:
    void pushState(AudioStrip* strip, bool committed);
    void showPicked();

    model::Project* project_{nullptr};
    model::SequenceId sequenceId_;
    edit::CommandStack* commands_{nullptr};
    bool updating_{false};

    QWidget* strips_{nullptr};
    std::vector<AudioStrip*> strip_;
    AudioStrip* master_{nullptr};
    LevelMeter* masterMeter_{nullptr};
    QLabel* soloLabel_{nullptr};
    model::TrackId picked_;
};

}  // namespace zaro::app
