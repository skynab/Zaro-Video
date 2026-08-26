#pragma once

#include <QElapsedTimer>
#include <QString>
#include <QWidget>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "zaro/core/model/Project.h"
#include "zaro/core/time/RationalTime.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSlider;
class QVBoxLayout;

namespace zaro::app {

class PillSwitch;

/// The Deliver workspace: what gets made, where it goes, and what is rendering.
///
/// Three columns, as the design has them -- presets, the settings for the one
/// that is picked, and the render queue. Everything on screen is something the
/// renderer actually honours: a menu offering a resolution or a frame rate this
/// program cannot change would be a promise the file then breaks, so the
/// sequence's own numbers are shown as facts rather than as controls.
class DeliverPanel : public QWidget {
    Q_OBJECT

public:
    explicit DeliverPanel(QWidget* parent = nullptr);
    ~DeliverPanel() override;

    /// Neither is owned. The project is read when a job is queued, not when it
    /// renders -- see `queueCurrent`.
    void setProject(const model::Project* project, model::SequenceId sequence);
    void setPlayhead(const time::RationalTime& position);

    /// Re-read what the sequence says: its size, its rate, its output curve.
    void refresh();

    /// Put the current settings on the queue as a job.
    void queueCurrent();
    /// Start rendering, or stop what is rendering now.
    void toggleRendering();
    [[nodiscard]] bool rendering() const noexcept { return rendering_; }
    /// One line for the window's status bar.
    [[nodiscard]] QString statusSummary() const;
    /// What the tool bar shows while this workspace is up.
    [[nodiscard]] QString rangeSummary() const;

    /// Where the next queued job writes. For the self-test, which cannot use a
    /// file dialog, and for anything else that wants to aim the panel.
    void setDestination(const QString& folder, const QString& name);
    [[nodiscard]] int queueLength() const { return static_cast<int>(jobs_.size()); }
    /// How many jobs finished cleanly.
    [[nodiscard]] int finishedCount() const;
    /// The last thing a job said, which is where a failure's reason ends up.
    [[nodiscard]] QString lastMessage() const;

signals:
    /// The queue, or what it is doing, changed: the window's chrome follows it.
    void queueChanged();

private:
    struct Preset;
    struct Job;

    void buildPresets(QVBoxLayout* into);
    void buildSettings(QVBoxLayout* into);
    void buildQueue(QVBoxLayout* into);

    void applyPreset(int index);
    void updateDerived();
    void rebuildQueueView();
    void startNext();
    void jobProgress(std::size_t index, std::int64_t done, std::int64_t total, double elapsed);
    void jobFinished(std::size_t index, bool ok, const QString& message);
    void stopCurrent();
    void chooseFolder();

    [[nodiscard]] const model::Sequence* sequence() const;
    /// The range the Range control asks for, as a start frame and a count.
    [[nodiscard]] std::pair<std::int64_t, std::int64_t> chosenRange() const;
    [[nodiscard]] std::int64_t bitRate() const;
    [[nodiscard]] std::string outputPath() const;

    const model::Project* project_{nullptr};
    model::SequenceId sequenceId_;
    time::RationalTime playhead_{};

    std::vector<Preset> presets_;
    std::vector<QListWidget*> presetLists_;
    int preset_{0};

    QLabel* presetName_{nullptr};
    QLabel* presetDescription_{nullptr};
    QLabel* estimateSize_{nullptr};
    QLabel* estimateBitrate_{nullptr};
    QLabel* estimateDuration_{nullptr};
    QLabel* estimateColour_{nullptr};

    QLineEdit* fileName_{nullptr};
    QLineEdit* folder_{nullptr};
    QLabel* fullPath_{nullptr};

    QComboBox* container_{nullptr};
    QComboBox* codec_{nullptr};
    QLabel* resolution_{nullptr};
    QLabel* frameRate_{nullptr};
    QLabel* depth_{nullptr};
    QSlider* quality_{nullptr};
    QLabel* qualityName_{nullptr};
    QLabel* qualityRate_{nullptr};

    PillSwitch* audioOn_{nullptr};
    QComboBox* audioCodec_{nullptr};
    QLabel* audioChannels_{nullptr};
    QLabel* audioRate_{nullptr};
    QLabel* loudness_{nullptr};

    PillSwitch* smartRender_{nullptr};
    PillSwitch* useGpu_{nullptr};
    PillSwitch* revealWhenDone_{nullptr};

    std::vector<QWidget*> rangeButtons_;
    int range_{0};

    QLabel* queueSummary_{nullptr};
    QVBoxLayout* queueLayout_{nullptr};
    QLabel* machineSpace_{nullptr};
    QLabel* machineJob_{nullptr};
    QLabel* machineSpeed_{nullptr};

    std::vector<std::unique_ptr<Job>> jobs_;
    bool rendering_{false};
    /// The job the worker is on, or -1.
    int active_{-1};

    /// How long since the queue's cards were last rebuilt, so progress from a
    /// render that reports every frame does not rebuild them every frame.
    QElapsedTimer progressClock_;

    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> shuttingDown_{false};
};

}  // namespace zaro::app
