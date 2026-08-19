#include "ProjectBin.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/media/Waveform.h"
#include "zaro/core/time/Timecode.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"

namespace zaro::app {
namespace {

QString describe(const model::MediaRef& ref) {
    QString text = QString::fromStdString(ref.name.empty() ? ref.path : ref.name);
    if (const media::VideoStreamInfo* video = ref.info.primaryVideo()) {
        text += QString("   %1x%2").arg(video->width).arg(video->height);
    }
    if (ref.info.duration.isPositive()) {
        text += QString("   %1s").arg(ref.info.duration.toDouble(), 0, 'f', 2);
    }
    if (ref.info.primaryAudio() != nullptr) {
        text += "   ♪";
    }
    return text;
}

}  // namespace

ProjectBin::ProjectBin(QWidget* parent) : QWidget{parent} {
    auto* title = new QLabel("Project", this);
    title->setStyleSheet("font-weight: 600;");

    list_ = new QListWidget(this);
    importButton_ = new QPushButton("Import…", this);
    // Short, because the bin is narrow and a clipped label is worse than a
    // terse one. Double-clicking an item does the same thing.
    auto* appendButton = new QPushButton("Append", this);
    appendButton->setToolTip("Append to the end of the timeline");

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(importButton_);
    buttons->addWidget(appendButton);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(title);
    layout->addWidget(list_, 1);
    layout->addLayout(buttons);

    connect(importButton_, &QPushButton::clicked, this, &ProjectBin::importFiles);
    connect(appendButton, &QPushButton::clicked, this, &ProjectBin::appendSelectedToTimeline);
    connect(list_, &QListWidget::itemDoubleClicked, this, [this] { appendSelectedToTimeline(); });
}

void ProjectBin::setProject(model::Project* project, model::SequenceId sequence,
                            edit::CommandStack* commands) {
    project_ = project;
    sequenceId_ = sequence;
    commands_ = commands;
    refresh();
}

void ProjectBin::refresh() {
    list_->clear();
    if (project_ == nullptr) {
        return;
    }
    for (const model::MediaRef& ref : project_->media()) {
        auto* item = new QListWidgetItem(describe(ref), list_);
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(ref.id.value()));
    }
}

void ProjectBin::importFiles() {
    if (project_ == nullptr || commands_ == nullptr) {
        return;
    }
    const QStringList chosen = QFileDialog::getOpenFileNames(this, "Import media");
    if (chosen.isEmpty()) {
        return;
    }

    // Probed on this thread rather than a background one. A probe reads a
    // header, not a stream -- it takes milliseconds -- and every background
    // thread added is another lifetime to get right, which is what caused the
    // abort-on-quit bug.
    for (const QString& path : chosen) {
        auto probed = zaro::platform::ffmpeg::probe(path.toStdString());
        if (!probed) {
            continue;
        }
        model::MediaRef ref;
        ref.path = path.toStdString();
        ref.name = QFileInfo(path).fileName().toStdString();
        ref.info = *probed;
        if (auto hash = media::quickContentHash(ref.path)) {
            ref.contentHash = *hash;
        }

        auto built = edit::makeImportMedia(*project_, std::move(ref));
        if (built) {
            commands_->execute(*project_, std::move(*built));
        }
    }
    commands_->breakMerge();
    refresh();
    emit edited();
}

void ProjectBin::appendSelectedToTimeline() {
    if (project_ == nullptr || commands_ == nullptr) {
        return;
    }
    QListWidgetItem* item = list_->currentItem();
    if (item == nullptr) {
        return;
    }
    const model::MediaRefId id{item->data(Qt::UserRole).toULongLong()};
    const model::MediaRef* ref = project_->findMedia(id);
    const model::Sequence* sequence = project_->findSequence(sequenceId_);
    if (ref == nullptr || sequence == nullptr || !ref->info.duration.isPositive()) {
        return;
    }

    const time::Rational& rate = sequence->frameRate();
    const media::VideoStreamInfo* video = ref->info.primaryVideo();
    const time::Rational sourceRate = video != nullptr ? video->frameRate : rate;

    const bool hasVideo = video != nullptr;
    const auto& tracks = hasVideo ? sequence->videoTracks() : sequence->audioTracks();
    if (tracks.empty()) {
        return;
    }
    const model::Track& track = tracks.front();

    // Appended after whatever is already there, which is what "append" means
    // and avoids having to decide what to overwrite.
    const time::RationalTime start =
        track.isEmpty() ? time::RationalTime{0, rate} : track.extent().endExclusive();
    const auto duration = time::RationalTime::fromSeconds(ref->info.duration, rate);
    if (duration.frames() <= 0) {
        return;
    }

    model::Clip clip;
    clip.id = project_->ids().next<model::ClipTag>();
    clip.source = id;
    clip.name = ref->name;
    clip.sourceRange =
        time::TimeRange{time::RationalTime{0, sourceRate},
                        time::RationalTime::fromSeconds(ref->info.duration, sourceRate)};
    clip.timelineRange = time::TimeRange{start, duration};

    auto built = edit::makeOverwrite(*project_, {sequenceId_, track.id()}, clip);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    emit edited();
}

}  // namespace zaro::app
