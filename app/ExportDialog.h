#pragma once

#include <QDialog>
#include <atomic>
#include <memory>
#include <thread>

#include "zaro/core/model/Project.h"

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QCheckBox;

namespace zaro::app {

/// Export settings and progress.
///
/// The render runs on its own thread, because it takes as long as it takes and
/// a dialog that stops responding is indistinguishable from one that has hung.
/// It is cancellable for the same reason, and a cancelled render deletes its
/// partial file rather than leaving something that looks like a delivery.
class ExportDialog : public QDialog {
    Q_OBJECT

public:
    ExportDialog(const model::Project& project, model::SequenceId sequence,
                 QWidget* parent = nullptr);
    ~ExportDialog() override;

private:
    void chooseFile();
    void start();
    void cancel();
    void finish(bool succeeded, const QString& message);

    const model::Project* project_;
    model::SequenceId sequenceId_;

    QLineEdit* path_{nullptr};
    QCheckBox* withAudio_{nullptr};
    QProgressBar* progress_{nullptr};
    QLabel* status_{nullptr};
    QPushButton* startButton_{nullptr};
    QPushButton* closeButton_{nullptr};

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelled_{false};
};

}  // namespace zaro::app
