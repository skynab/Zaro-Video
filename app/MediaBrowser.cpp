#include "MediaBrowser.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <filesystem>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/MediaBrowser.h"
#include "zaro/core/media/Waveform.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"

namespace zaro::app {
namespace {

/// A size somebody can read at a glance rather than count digits in.
QString readable(std::uint64_t bytes) {
    const double megabytes = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (megabytes >= 1024.0) {
        return QString("%1 GB").arg(megabytes / 1024.0, 0, 'f', 1);
    }
    return QString("%1 MB").arg(megabytes, 0, 'f', 1);
}

}  // namespace

MediaBrowser::MediaBrowser(QWidget* parent) : QDialog{parent} {
    setWindowTitle("Browse media");
    resize(560, 420);

    path_ = new QLabel(this);
    path_->setProperty("muted", true);
    path_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* up = new QPushButton("Up", this);
    up->setObjectName("browser-up");
    auto* choose = new QPushButton("Choose Folder…", this);
    choose->setObjectName("browser-choose");

    list_ = new QListWidget(this);
    list_->setObjectName("browser-list");
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);

    importButton_ = new QPushButton("Import Selected", this);
    importButton_->setObjectName("browser-import");
    auto* close = new QPushButton("Close", this);

    footer_ = new QLabel(this);
    footer_->setProperty("muted", true);

    auto* header = new QHBoxLayout;
    header->addWidget(up);
    header->addWidget(choose);
    header->addWidget(path_, 1);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(footer_, 1);
    buttons->addWidget(importButton_);
    buttons->addWidget(close);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(header);
    layout->addWidget(list_, 1);
    layout->addLayout(buttons);

    connect(up, &QPushButton::clicked, this, [this] { goUp(); });
    connect(choose, &QPushButton::clicked, this, [this] {
        const QString chosen =
            QFileDialog::getExistingDirectory(this, "Browse", QString::fromStdString(folder_));
        if (!chosen.isEmpty()) {
            static_cast<void>(showFolder(chosen.toStdString()));
        }
    });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this] { openSelected(); });
    connect(importButton_, &QPushButton::clicked, this, [this] {
        auto added = importSelected();
        if (added && *added > 0) {
            emit imported();
        }
    });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
}

void MediaBrowser::bind(const ui::SequenceBinding& binding) {
    project_ = binding.project;
    commands_ = binding.commands;
}

Status MediaBrowser::showFolder(const std::string& path) {
    auto listed = io::listFolder(path);
    if (!listed) {
        return listed.error();
    }
    folder_ = std::filesystem::absolute(path).lexically_normal().string();
    path_->setText(QString::fromStdString(folder_));
    list_->clear();

    int files = 0;
    for (const io::FolderEntry& entry : *listed) {
        // Folders marked rather than coloured: what somebody is scanning for
        // is "can I go in there", and a symbol survives any theme.
        auto* item = new QListWidgetItem(
            entry.isFolder ? QString("▸ %1").arg(QString::fromStdString(entry.name))
                           : QString("   %1   %2")
                                 .arg(QString::fromStdString(entry.name), readable(entry.bytes)),
            list_);
        item->setData(Qt::UserRole, QString::fromStdString(entry.path));
        item->setData(Qt::UserRole + 1, entry.isFolder);
        files += entry.isFolder ? 0 : 1;
    }
    footer_->setText(files == 1 ? QString("1 file") : QString("%1 files").arg(files));
    return {};
}

void MediaBrowser::selectAllFiles() {
    list_->clearSelection();
    for (int row = 0; row < list_->count(); ++row) {
        if (!list_->item(row)->data(Qt::UserRole + 1).toBool()) {
            list_->item(row)->setSelected(true);
        }
    }
}

void MediaBrowser::goUp() {
    if (folder_.empty()) {
        return;
    }
    const std::filesystem::path parent = std::filesystem::path{folder_}.parent_path();
    if (parent.empty() || parent == folder_) {
        return;  // already at the root; there is nowhere further up
    }
    static_cast<void>(showFolder(parent.string()));
}

void MediaBrowser::openSelected() {
    QListWidgetItem* item = list_->currentItem();
    if (item == nullptr || !item->data(Qt::UserRole + 1).toBool()) {
        return;
    }
    static_cast<void>(showFolder(item->data(Qt::UserRole).toString().toStdString()));
}

Result<int> MediaBrowser::importSelected() {
    if (project_ == nullptr || commands_ == nullptr) {
        return Error{ErrorCode::InvalidData, "there is no project to import into"};
    }
    int added = 0;
    for (QListWidgetItem* item : list_->selectedItems()) {
        if (item->data(Qt::UserRole + 1).toBool()) {
            continue;  // a folder is somewhere to go, not something to import
        }
        const std::string path = item->data(Qt::UserRole).toString().toStdString();

        // Already in the project? Importing it twice would give two entries
        // pointing at one file, which is two things to grade and relink.
        const bool known =
            std::any_of(project_->media().begin(), project_->media().end(),
                        [&path](const model::MediaRef& ref) { return ref.path == path; });
        if (known) {
            continue;
        }

        auto probed = platform::ffmpeg::probe(path);
        if (!probed) {
            continue;  // listed by extension, and the extension was wrong
        }
        model::MediaRef ref;
        ref.path = path;
        ref.name = std::filesystem::path{path}.filename().string();
        ref.info = *probed;
        if (auto hash = media::quickContentHash(path)) {
            ref.contentHash = *hash;
        }
        if (auto digest = media::contentDigest(path)) {
            ref.contentDigest = *digest;
        }
        auto built = edit::makeImportMedia(*project_, std::move(ref));
        if (!built) {
            continue;
        }
        commands_->execute(*project_, std::move(*built));
        ++added;
    }
    if (added > 0) {
        commands_->breakMerge();
    }
    return added;
}

}  // namespace zaro::app
