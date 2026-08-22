#include "ProjectBin.h"

#include <QApplication>
#include <QCursor>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <filesystem>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/media/Waveform.h"
#include "zaro/core/model/MediaSearch.h"
#include "zaro/core/time/Timecode.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"

namespace zaro::app {
namespace {

QString describe(const model::Subclip& subclip, const model::MediaRef& source) {
    // Indented and named after its own label rather than the file: a subclip is
    // a note about where the good part is, and the file it came from is the
    // less interesting half of that.
    QString text =
        QString("    ") + QString::fromStdString(subclip.name.empty() ? source.name : subclip.name);
    text += QString("   %1s").arg(subclip.range.duration().toSecondsDouble(), 0, 'f', 2);
    return text;
}

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
    if (const media::VideoStreamInfo* video = ref.info.primaryVideo();
        video != nullptr && !video->codecName.empty()) {
        // The codec, because "which of these is the ProRes" is a question
        // people ask of a bin, and because a search that matches on it should
        // show what it matched.
        text += QString("   %1").arg(QString::fromStdString(video->codecName));
    }
    if (!ref.notes.empty()) {
        text += QString("   \u2014 %1").arg(QString::fromStdString(ref.notes));
    }
    if (ref.transferOverride != media::TransferFunction::Unknown) {
        // Shown, because a file being read as something other than what it
        // claims is exactly the kind of setting somebody forgets they made.
        text += QString("   [%1]").arg(QString::fromUtf8(media::toString(ref.transferOverride)));
    }
    return text;
}

}  // namespace

ProjectBin::ProjectBin(QWidget* parent) : QWidget{parent} {
    auto* title = new QLabel("Media", this);
    title->setStyleSheet("font-weight: 600;");

    list_ = new QListWidget(this);
    importButton_ = new QPushButton("Import…", this);

    // A filter rather than a second panel. A bin is looked *in*, and typing
    // three letters of a file name is how: at thirty clips the list is already
    // longer than the panel is tall.
    search_ = new QLineEdit(this);
    search_->setPlaceholderText("Search name, codec, size, notes");
    search_->setClearButtonEnabled(true);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) {
        filter_ = text.trimmed();
        applyFilter();
    });

    // Short, because the bin is narrow and a clipped label is worse than a
    // terse one. Double-clicking an item does the same thing.
    auto* appendButton = new QPushButton("Append", this);
    appendButton->setToolTip("Append to the end of the timeline");
    auto* interpretButton = new QPushButton("Interpret…", this);
    interpretButton->setObjectName("bin-interpret");
    interpretButton->setToolTip("Say what this footage's curve really is");

    auto* replaceButton = new QPushButton("Replace", this);
    replaceButton->setObjectName("bin-replace");
    replaceButton->setToolTip("Point the selected timeline clip at this media instead");

    auto* notesButton = new QPushButton("Notes…", this);
    notesButton->setObjectName("bin-notes");
    notesButton->setToolTip("Write something about this file -- it is searchable");

    // Two by two rather than four across: at the width this panel actually
    // gets, a single row clipped every label to two letters.
    auto* buttons = new QGridLayout;
    buttons->setSpacing(6);
    buttons->addWidget(appendButton, 0, 0);
    buttons->addWidget(replaceButton, 0, 1);
    buttons->addWidget(interpretButton, 1, 0);
    buttons->addWidget(notesButton, 1, 1);

    auto* header = new QHBoxLayout;
    header->addWidget(title);
    header->addStretch(1);
    header->addWidget(importButton_);

    footer_ = new QLabel(this);
    footer_->setProperty("muted", true);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(6);
    layout->addLayout(header);
    layout->addWidget(search_);
    layout->addWidget(list_, 1);
    layout->addLayout(buttons);
    layout->addWidget(footer_);

    connect(importButton_, &QPushButton::clicked, this, &ProjectBin::importFiles);
    connect(appendButton, &QPushButton::clicked, this, &ProjectBin::appendSelectedToTimeline);
    connect(list_, &QListWidget::itemDoubleClicked, this, [this] { appendSelectedToTimeline(); });
    connect(list_, &QListWidget::currentRowChanged, this, [this] {
        const Selection chosen = selection();
        if (!chosen.media.isValid()) {
            return;
        }
        if (chosen.subclip.isValid()) {
            emit openSubclipRequested(chosen.subclip);
        } else {
            emit openRequested(chosen.media);
        }
    });
    connect(interpretButton, &QPushButton::clicked, this, [this] { interpretMenu(); });
    connect(notesButton, &QPushButton::clicked, this, [this] { editNotes(); });
    connect(importButton_, &QPushButton::customContextMenuRequested, this, [this] {
        // Right-click on Import, because transcoding on the way in is the
        // less common choice and does not deserve a button of its own in a
        // panel this narrow.
        QMenu menu;
        QAction* plain = menu.addAction("Import…");
        QAction* transcoded = menu.addAction("Import and transcode to ProRes…");
        QAction* chosen = menu.exec(QCursor::pos());
        if (chosen == plain) {
            importFiles();
        } else if (chosen == transcoded) {
            importTranscodedDialog();
        }
    });
    importButton_->setContextMenuPolicy(Qt::CustomContextMenu);
    importButton_->setToolTip("Import media — right-click to transcode on the way in");
    connect(replaceButton, &QPushButton::clicked, this, [this] {
        const Selection chosen = selection();
        if (chosen.media.isValid()) {
            emit replaceRequested(chosen.media);
        }
    });
}

/// Hide what the search does not match, rather than rebuilding the list: the
/// selection survives typing, which is what makes narrowing down feel like
/// looking rather than starting again.
///
/// The match itself is `model::matchesSearch`, not a substring test on the row
/// text: what is findable should be everything the project knows about a file
/// -- its codec, its size, its rate, the folder it came from, the notes on it
/// -- and not merely the part of that which happens to fit on one line.
void ProjectBin::applyFilter() {
    const std::string query = filter_.toStdString();
    int shown = 0;
    for (int row = 0; row < list_->count(); ++row) {
        QListWidgetItem* item = list_->item(row);
        const model::MediaRef* ref =
            project_ != nullptr
                ? project_->findMedia(model::MediaRefId{item->data(Qt::UserRole).toULongLong()})
                : nullptr;
        const bool matches = filter_.isEmpty() ||
                             (ref != nullptr ? model::matchesSearch(*ref, query)
                                             : item->text().contains(filter_, Qt::CaseInsensitive));
        item->setHidden(!matches);
        shown += matches ? 1 : 0;
    }
    footer_->setText(
        filter_.isEmpty()
            ? QString("%1 %2").arg(list_->count()).arg(list_->count() == 1 ? "item" : "items")
            : QString("%1 of %2 items").arg(shown).arg(list_->count()));
}

/// Ask for a note about the selected file, and keep it.
///
/// The note goes through a command, so it undoes and so a project with a note
/// in it reads as modified -- the alternative is somebody typing a note,
/// quitting, and being told there was nothing to save.
void ProjectBin::editNotes() {
    const Selection chosen = selection();
    if (project_ == nullptr || commands_ == nullptr || !chosen.media.isValid()) {
        return;
    }
    const model::MediaRef* ref = project_->findMedia(chosen.media);
    if (ref == nullptr) {
        return;
    }
    bool accepted = false;
    const QString typed = QInputDialog::getText(
        this, "Notes", QString("Notes on %1").arg(QString::fromStdString(ref->name)),
        QLineEdit::Normal, QString::fromStdString(ref->notes), &accepted);
    if (!accepted) {
        return;
    }
    setNotes(chosen.media, typed.toStdString());
}

void ProjectBin::setNotes(model::MediaRefId media, const std::string& notes) {
    if (project_ == nullptr || commands_ == nullptr) {
        return;
    }
    auto built = edit::makeSetMediaNotes(*project_, media, notes);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    refresh();
    emit edited();
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
        applyFilter();
        return;
    }
    for (const model::MediaRef& ref : project_->media()) {
        auto* item = new QListWidgetItem(describe(ref), list_);
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(ref.id.value()));
        item->setData(Qt::UserRole + 1, QVariant::fromValue<qulonglong>(0));
        // Subclips of this file, under it. Grouped rather than listed
        // separately, because what somebody looks for is the take, and the take
        // is found by finding the file it is in.
        for (const model::Subclip& subclip : project_->subclips()) {
            if (subclip.source != ref.id) {
                continue;
            }
            auto* child = new QListWidgetItem(describe(subclip, ref), list_);
            child->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(ref.id.value()));
            child->setData(Qt::UserRole + 1, QVariant::fromValue<qulonglong>(subclip.id.value()));
        }
    }
    applyFilter();
}

int ProjectBin::count() const {
    return project_ != nullptr ? static_cast<int>(project_->media().size()) : 0;
}

/// Import files, transcoding each into an editing codec on the way in.
///
/// **The transcode is the media, not a proxy.** A proxy stands in for a file
/// that stays where it is; an ingest transcode replaces it, because the reason
/// to do it is that the camera's own codec is painful to cut with. The
/// original is left exactly where it was -- ingesting must not be a thing that
/// eats rushes -- and its path is written into the notes, which is the only
/// record of where this came from once the project points at the copy.
///
/// The transcode itself is `makeProxy` with the size left alone: a proxy and an
/// ingest transcode are one operation with different settings.
Status ProjectBin::importTranscoded(const std::vector<std::string>& paths,
                                    const std::string& destination, const std::string& videoCodec) {
    if (project_ == nullptr || commands_ == nullptr) {
        return Error{ErrorCode::InvalidData, "there is no project to import into"};
    }
    std::error_code code;
    std::filesystem::create_directories(destination, code);
    if (!std::filesystem::is_directory(destination, code)) {
        return Error{ErrorCode::Io, "cannot use " + destination + " as a folder"};
    }

    for (const std::string& path : paths) {
        const std::filesystem::path source{path};
        platform::ffmpeg::ProxySettings settings;
        settings.source = path;
        settings.destination =
            (std::filesystem::path{destination} / (source.stem().string() + ".mov")).string();
        settings.width = 0;  // the source's own size
        settings.videoCodec = videoCodec;

        auto made = platform::ffmpeg::makeProxy(settings);
        if (!made) {
            return made.error();
        }
        auto probed = platform::ffmpeg::probe(made->path);
        if (!probed) {
            return probed.error();
        }

        model::MediaRef ref;
        ref.path = made->path;
        ref.name = source.stem().string();
        ref.info = *probed;
        ref.notes = "ingested from " + path;
        if (auto hash = media::quickContentHash(ref.path)) {
            ref.contentHash = *hash;
        }
        if (auto digest = media::contentDigest(ref.path)) {
            ref.contentDigest = *digest;
        }
        auto built = edit::makeImportMedia(*project_, std::move(ref));
        if (!built) {
            return built.error();
        }
        commands_->execute(*project_, std::move(*built));
    }
    commands_->breakMerge();
    refresh();
    emit edited();
    return {};
}

void ProjectBin::importTranscodedDialog() {
    const QStringList chosen = QFileDialog::getOpenFileNames(this, "Import and transcode");
    if (chosen.isEmpty()) {
        return;
    }
    const QString into = QFileDialog::getExistingDirectory(this, "Put the transcoded files in");
    if (into.isEmpty()) {
        return;
    }
    std::vector<std::string> paths;
    paths.reserve(static_cast<std::size_t>(chosen.size()));
    for (const QString& path : chosen) {
        paths.push_back(path.toStdString());
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const Status done = importTranscoded(paths, into.toStdString(), "prores_ks");
    QApplication::restoreOverrideCursor();
    if (!done) {
        QMessageBox::warning(this, "Import", QString::fromStdString(done.error().message()));
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
        // Taken at import, because it is the only moment the file is certainly
        // where the project thinks it is -- and a relink with nothing to
        // compare against can only match on names.
        if (auto digest = media::contentDigest(ref.path)) {
            ref.contentDigest = *digest;
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

ProjectBin::Selection ProjectBin::selection() const {
    QListWidgetItem* item = list_->currentItem();
    if (item == nullptr) {
        return {};
    }
    return Selection{model::MediaRefId{item->data(Qt::UserRole).toULongLong()},
                     model::SubclipId{item->data(Qt::UserRole + 1).toULongLong()}};
}

/// Say what a file's curve really is.
///
/// In the bin because it is a fact about the file, and the bin is the list of
/// files. Not a guess this program could make for somebody: a flat shot and a
/// log shot are the same picture, and the only thing that can tell them apart
/// is a person who knows what the camera was set to.
void ProjectBin::interpretMenu() {
    const Selection chosen = selection();
    const model::MediaRef* ref = project_ != nullptr ? project_->findMedia(chosen.media) : nullptr;
    if (ref == nullptr) {
        return;
    }

    QMenu menu;
    std::map<QAction*, media::TransferFunction> entries;
    for (const media::TransferFunction transfer : media::allTransferFunctions()) {
        QAction* action =
            menu.addAction(transfer == media::TransferFunction::Unknown
                               ? QString("As the file says (%1)")
                                     .arg(QString::fromUtf8(media::toString(ref->transfer())))
                               : QString::fromUtf8(media::toString(transfer)));
        action->setCheckable(true);
        action->setChecked(ref->transferOverride == transfer);
        entries.emplace(action, transfer);
    }

    QAction* picked = menu.exec(QCursor::pos());
    const auto found = entries.find(picked);
    if (found == entries.end()) {
        return;
    }
    for (model::MediaRef& media : project_->mediaMutable()) {
        if (media.id == chosen.media) {
            media.transferOverride = found->second;
        }
    }
    refresh();
    emit colorChanged();
}

void ProjectBin::appendSelectedToTimeline() {
    if (project_ == nullptr || commands_ == nullptr) {
        return;
    }
    const Selection chosen = selection();
    const model::MediaRefId id = chosen.media;
    const model::MediaRef* ref = project_->findMedia(id);
    const model::Sequence* sequence = project_->findSequence(sequenceId_);
    if (ref == nullptr || sequence == nullptr || !ref->info.duration.isPositive()) {
        return;
    }

    // The first thing on an empty timeline decides its format.
    //
    // Done here, before anything below takes a reference into the sequence: a
    // command replaces the sequence wholesale, so a rate or a track captured
    // first would be left pointing at the version that has just been thrown
    // away. That is the bug this project has already found twice.
    //
    // In the bin rather than in the model, because it is a decision about what
    // somebody meant, and those belong where the interaction is; every edit
    // operation would otherwise have to carry a rule about when a sequence may
    // change shape. The operation refuses once there is anything to retime, so
    // calling it again later cannot do harm.
    if (const media::VideoStreamInfo* first = ref->info.primaryVideo();
        first != nullptr && sequence->duration().frames() == 0) {
        auto conformed = edit::makeConformSequence(*project_, sequenceId_, first->frameRate,
                                                   first->width, first->height);
        if (conformed) {
            commands_->execute(*project_, std::move(*conformed));
            commands_->breakMerge();
            sequence = project_->findSequence(sequenceId_);
        }
    }

    const time::Rational& rate = sequence->frameRate();
    const media::VideoStreamInfo* video = ref->info.primaryVideo();
    const time::Rational sourceRate = video != nullptr ? video->frameRate : rate;

    // A subclip appends its range; media appends all of it. This is the only
    // place a subclip means anything: what lands on the timeline is an
    // ordinary clip either way.
    const model::Subclip* subclip = project_->findSubclip(chosen.subclip);
    const time::TimeRange sourceRange =
        subclip != nullptr
            ? subclip->range.rescaledTo(sourceRate)
            : time::TimeRange{time::RationalTime{0, sourceRate},
                              time::RationalTime::fromSeconds(ref->info.duration, sourceRate)};

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
    const auto duration = sourceRange.duration().rescaledTo(rate);
    if (duration.frames() <= 0) {
        return;
    }

    model::Clip clip;
    clip.id = project_->ids().next<model::ClipTag>();
    clip.source = id;
    clip.name = subclip != nullptr && !subclip->name.empty() ? subclip->name : ref->name;
    clip.sourceRange = sourceRange;
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
