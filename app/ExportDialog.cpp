#include "ExportDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <filesystem>

#include "zaro/platform/ffmpeg/FFmpegRender.h"
#include "zaro/platform/qtext/QtTextRasterizer.h"

namespace zaro::app {

ExportDialog::ExportDialog(const model::Project& project, model::SequenceId sequence,
                           QWidget* parent)
    : QDialog{parent}, project_{&project}, sequenceId_{sequence} {
    setWindowTitle("Export");
    setModal(true);

    path_ = new QLineEdit(this);
    path_->setPlaceholderText("Choose a destination…");
    auto* browse = new QPushButton("Browse…", this);

    auto* pathRow = new QHBoxLayout;
    pathRow->addWidget(path_, 1);
    pathRow->addWidget(browse);

    withAudio_ = new QCheckBox("Include audio", this);
    withAudio_->setChecked(true);

    progress_ = new QProgressBar(this);
    progress_->setRange(0, 100);
    status_ = new QLabel(this);

    startButton_ = new QPushButton("Export", this);
    closeButton_ = new QPushButton("Close", this);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(startButton_);
    buttons->addWidget(closeButton_);

    auto* form = new QFormLayout;
    form->addRow("File", pathRow);
    form->addRow("", withAudio_);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(progress_);
    layout->addWidget(status_);
    layout->addLayout(buttons);

    if (const model::Sequence* seq = project_->findSequence(sequenceId_)) {
        status_->setText(QString("%1 frames at %2")
                             .arg(seq->duration().frames())
                             .arg(QString::fromStdString(seq->frameRate().toString())));
    }

    connect(browse, &QPushButton::clicked, this, &ExportDialog::chooseFile);
    connect(startButton_, &QPushButton::clicked, this, &ExportDialog::start);
    connect(closeButton_, &QPushButton::clicked, this, [this] {
        if (running_.load(std::memory_order_relaxed)) {
            cancel();
        } else {
            accept();
        }
    });
    resize(520, 200);
}

ExportDialog::~ExportDialog() {
    // Same reasoning as the preview window: a std::thread destroyed while still
    // joinable calls std::terminate, and closing a dialog mid-render is an
    // entirely ordinary thing to do.
    cancelled_.store(true, std::memory_order_relaxed);
    if (worker_.joinable()) {
        worker_.join();
    }
}

void ExportDialog::chooseFile() {
    const QString chosen = QFileDialog::getSaveFileName(this, "Export to", path_->text(),
                                                        "QuickTime (*.mov);;MPEG-4 (*.mp4)");
    if (!chosen.isEmpty()) {
        path_->setText(chosen);
    }
}

void ExportDialog::start() {
    if (running_.load(std::memory_order_relaxed)) {
        return;
    }
    const QString destination = path_->text();
    if (destination.isEmpty()) {
        status_->setText("Choose a destination first.");
        return;
    }

    platform::ffmpeg::RenderRequest request;
    request.outputPath = destination.toStdString();
    request.sequence = sequenceId_;
    request.includeAudio = withAudio_->isChecked();

    cancelled_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    startButton_->setEnabled(false);
    closeButton_->setText("Cancel");
    status_->setText("Rendering…");

    if (worker_.joinable()) {
        worker_.join();
    }
    worker_ = std::thread{[this, request] {
        const auto keepGoing = [this] { return !cancelled_.load(std::memory_order_relaxed); };
        const auto onProgress = [this](const platform::ffmpeg::RenderProgress& progress) {
            const int percent =
                progress.framesTotal > 0
                    ? static_cast<int>(progress.framesDone * 100 / progress.framesTotal)
                    : 0;
            const double fps =
                progress.elapsedSeconds > 0.0
                    ? static_cast<double>(progress.framesDone) / progress.elapsedSeconds
                    : 0.0;
            // Back to the UI thread: widgets are not thread safe.
            QMetaObject::invokeMethod(
                this,
                [this, percent, progress, fps] {
                    progress_->setValue(percent);
                    status_->setText(QString("%1 of %2 frames — %3 fps")
                                         .arg(progress.framesDone)
                                         .arg(progress.framesTotal)
                                         .arg(fps, 0, 'f', 1));
                },
                Qt::QueuedConnection);
        };

        // The same font engine the preview uses, so what is exported is what was
        // on screen. A dialog that rendered without one would produce a file
        // quietly missing its titles.
        platform::qtext::QtTextRasterizer text;
        platform::ffmpeg::RenderSummary summary;
        const Status status = platform::ffmpeg::renderSequence(*project_, request, onProgress,
                                                               keepGoing, &summary, &text);
        const bool cancelled = !status && status.error().code() == ErrorCode::Cancelled;
        // Said either way. An export that quietly copied would leave somebody
        // wondering where the grade went, and one that quietly re-encoded
        // would leave them wondering why it took twenty minutes.
        const QString how =
            summary.copied ? QString("Done — copied without re-encoding.")
                           : QString("Done — re-encoded (%1).")
                                 .arg(QString::fromStdString(summary.copyReason.empty()
                                                                 ? std::string{"nothing to copy"}
                                                                 : summary.copyReason));
        const QString message = status ? how : QString::fromStdString(status.error().toString());

        if (cancelled || !status) {
            // A partial file looks exactly like a finished one until someone
            // plays it, so it does not get left behind.
            std::error_code code;
            std::filesystem::remove(request.outputPath, code);
        }
        QMetaObject::invokeMethod(
            this, [this, ok = static_cast<bool>(status), message] { finish(ok, message); },
            Qt::QueuedConnection);
    }};
}

void ExportDialog::cancel() {
    cancelled_.store(true, std::memory_order_relaxed);
    status_->setText("Cancelling…");
}

void ExportDialog::finish(bool succeeded, const QString& message) {
    running_.store(false, std::memory_order_relaxed);
    startButton_->setEnabled(true);
    closeButton_->setText("Close");
    status_->setText(message);
    if (succeeded) {
        progress_->setValue(100);
    }
}

}  // namespace zaro::app
