#include "GalleryPanel.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

#include "Icons.h"
#include "Theme.h"

namespace zaro::app {
namespace {

constexpr int kThumbWidth = 100;
constexpr int kThumbHeight = 56;

/// A small swatch for a look, from its name.
///
/// Not a rendering of the cube: reading a LUT to draw a sixteen-pixel square
/// would be a file read per row of a list somebody is scrolling. The colour is
/// the name's hash, which is stable -- the same look is the same colour every
/// time it is listed -- and says nothing it does not know.
QPixmap swatchFor(const QString& name) {
    uint hue = static_cast<uint>(qHash(name) % 360U);
    QPixmap pixmap{16, 16};
    pixmap.fill(Qt::transparent);
    QPainter painter{&pixmap};
    painter.setRenderHint(QPainter::Antialiasing, true);
    QLinearGradient wash{QPointF{0, 0}, QPointF{16, 16}};
    wash.setColorAt(0.0, QColor::fromHsl(static_cast<int>(hue), 130, 120));
    wash.setColorAt(1.0, QColor::fromHsl(static_cast<int>((hue + 40) % 360), 90, 60));
    painter.setPen(Qt::NoPen);
    painter.setBrush(wash);
    painter.drawRoundedRect(QRectF{0, 0, 16, 16}, 3, 3);
    return pixmap;
}

/// A section heading with a button on the end of it, as the design draws both
/// the Gallery and the LUTs rows.
QWidget* heading(QWidget* parent, const QString& title, QLabel** note, QPushButton** action,
                 icons::Glyph glyph, const QString& tip) {
    auto* bar = new QFrame(parent);
    bar->setObjectName("gallery-heading");
    bar->setFixedHeight(32);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(10, 0, 6, 0);
    row->setSpacing(8);
    auto* label = new QLabel(title, bar);
    label->setObjectName("gallery-title");
    row->addWidget(label);
    if (note != nullptr) {
        *note = new QLabel(bar);
        (*note)->setProperty("muted", true);
        row->addWidget(*note);
    }
    row->addStretch(1);
    *action = new QPushButton(bar);
    (*action)->setObjectName("bin-glyph-button");
    (*action)->setIcon(icons::toolIcon(glyph, 13));
    (*action)->setFixedSize(24, 22);
    (*action)->setToolTip(tip);
    row->addWidget(*action);
    return bar;
}

}  // namespace

GalleryPanel::GalleryPanel(QWidget* parent) : QWidget{parent} {
    setObjectName("gallery-panel");
    setAttribute(Qt::WA_StyledBackground, true);

    QPushButton* grab = nullptr;
    QWidget* stillsHeading = heading(this, "Gallery", &count_, &grab, icons::Glyph::Camera,
                                     "Grab a still of this frame");
    connect(grab, &QPushButton::clicked, this, [this] { emit grabRequested(); });

    // An icon grid rather than hand-drawn tiles: two columns of thumbnails is
    // exactly what a list view in icon mode already is, and the alternative is
    // a second scrolling implementation to keep right.
    grid_ = new QListWidget(this);
    grid_->setObjectName("gallery-grid");
    grid_->setFrameShape(QFrame::NoFrame);
    grid_->setViewMode(QListView::IconMode);
    grid_->setIconSize(QSize{kThumbWidth, kThumbHeight});
    grid_->setResizeMode(QListView::Adjust);
    grid_->setMovement(QListView::Static);
    grid_->setSpacing(4);
    grid_->setWordWrap(true);
    grid_->setFixedHeight(238);
    connect(grid_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        const auto at = static_cast<std::size_t>(grid_->row(item));
        if (at < stills_.size()) {
            emit stillChosen(stills_[at].at);
        }
    });

    QPushButton* browse = nullptr;
    QWidget* lutHeading = heading(this, "LUTs", nullptr, &browse, icons::Glyph::FolderOpen,
                                  "Choose a folder of .cube looks");
    connect(browse, &QPushButton::clicked, this, [this] { chooseLutFolder(); });

    luts_ = new QListWidget(this);
    luts_->setObjectName("gallery-luts");
    luts_->setFrameShape(QFrame::NoFrame);
    luts_->setIconSize(QSize{16, 16});
    connect(luts_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) { emit lutChosen(item->data(Qt::UserRole).toString()); });

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    column->addWidget(stillsHeading);
    column->addWidget(grid_);
    column->addWidget(lutHeading);
    column->addWidget(luts_, 1);

    count_->setText("no stills");
}

int GalleryPanel::stillCount() const {
    return static_cast<int>(stills_.size());
}

void GalleryPanel::addStill(const QImage& frame, const time::RationalTime& at,
                            const QString& name) {
    if (frame.isNull()) {
        return;
    }
    stills_.push_back(Still{frame.scaled(QSize{kThumbWidth, kThumbHeight}, Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation),
                            at, name});
    auto* item = new QListWidgetItem(QIcon{QPixmap::fromImage(stills_.back().frame)}, name, grid_);
    item->setToolTip(QString("%1 — click to compare against this frame").arg(name));
    item->setSizeHint(QSize{kThumbWidth + 8, kThumbHeight + 22});
    count_->setText(
        QString("%1 %2").arg(stills_.size()).arg(stills_.size() == 1 ? "still" : "stills"));
}

void GalleryPanel::chooseLutFolder() {
    const QString folder =
        QFileDialog::getExistingDirectory(this, "Folder of .cube looks", folder_);
    if (!folder.isEmpty()) {
        showLutFolder(folder);
    }
}

void GalleryPanel::showLutFolder(const QString& folder) {
    folder_ = folder;
    luts_->clear();
    const QFileInfoList found =
        QDir{folder}.entryInfoList(QStringList{"*.cube", "*.CUBE"}, QDir::Files, QDir::Name);
    for (const QFileInfo& file : found) {
        auto* item = new QListWidgetItem(QIcon{swatchFor(file.fileName())}, file.fileName(), luts_);
        item->setData(Qt::UserRole, file.absoluteFilePath());
        // The size, because a 65-cube and a 17-cube are different enough that
        // somebody choosing between two similarly named looks wants to know.
        item->setToolTip(QString("%1\n%2 kB — double-click to apply")
                             .arg(file.absoluteFilePath())
                             .arg(file.size() / 1024));
    }
    if (found.isEmpty()) {
        auto* item = new QListWidgetItem("No .cube files here", luts_);
        item->setFlags(Qt::NoItemFlags);
    }
}

}  // namespace zaro::app
