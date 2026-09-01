#include "ProjectBin.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCursor>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStackedWidget>
#include <QStringList>
#include <QStyledItemDelegate>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <vector>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/MediaBrowser.h"
#include "zaro/core/media/Waveform.h"
#include "zaro/core/model/MediaSearch.h"
#include "zaro/core/time/Timecode.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"

#include "Icons.h"
#include "Say.h"
#include "Theme.h"
#include "chrome/FlowLayout.h"

namespace zaro::app {
namespace {

// What a row carries. The item's own text stays the file's name, so type-ahead
// and accessibility still find rows by the thing they are called; everything
// the delegate draws beyond that is here.
constexpr int kRoleMedia = Qt::UserRole;        ///< qulonglong: the media id
constexpr int kRoleSubclip = Qt::UserRole + 1;  ///< qulonglong: the subclip id, or 0
constexpr int kRoleHeader = Qt::UserRole + 2;   ///< bool: a folder heading
constexpr int kRoleMeta = Qt::UserRole + 3;     ///< the second line
constexpr int kRoleBadge = Qt::UserRole + 4;    ///< the duration, over the thumbnail
constexpr int kRoleGlyph = Qt::UserRole + 5;    ///< int: which icons::Glyph
constexpr int kRoleUsed = Qt::UserRole + 6;     ///< bool: the cut uses this
constexpr int kRoleCount = Qt::UserRole + 7;    ///< a heading's tally
constexpr int kRoleBin = Qt::UserRole + 8;      ///< which folder this row is under
constexpr int kRoleFolded = Qt::UserRole + 9;   ///< bool: a shut heading

// The row geometry the design draws, in logical pixels.
constexpr int kThumbWidth = 64;
constexpr int kThumbHeight = 38;
constexpr int kRowHeight = 50;
constexpr int kCompactRowHeight = 24;
constexpr int kHeaderHeight = 26;
constexpr int kGutter = 6;

/// The folder a file came from, which is the only grouping a project actually
/// has. The design's bins -- Interview, Drone, B-roll -- are how footage
/// arrives off a card, so the folder is not a stand-in for bins so much as
/// where bins come from.
QString binNameFor(const std::string& path) {
    const std::filesystem::path file{path};
    const std::filesystem::path folder = file.parent_path().filename();
    return folder.empty() ? QStringLiteral("Media") : QString::fromStdString(folder.string());
}

icons::Glyph glyphFor(const model::MediaRef& ref) {
    if (ref.info.primaryVideo() == nullptr) {
        return icons::Glyph::Waveform;
    }
    // A still has a picture and no running time; a movie has both.
    return ref.info.duration.isPositive() ? icons::Glyph::FilmStrip : icons::Glyph::Image;
}

/// Minutes and seconds, as the design's badge writes them. Not timecode: the
/// badge is four characters wide and a running time is what it is for.
QString badgeFor(const time::Rational& duration) {
    if (!duration.isPositive()) {
        return {};
    }
    const int total = static_cast<int>(std::lround(duration.toDouble()));
    return QString("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QChar('0'));
}

/// The second line: what this file is, in the order somebody scanning a bin
/// reads it -- codec first, because "which of these is the ProRes" is the
/// question a bin gets asked.
QString metaFor(const model::MediaRef& ref) {
    QStringList parts;
    if (const media::VideoStreamInfo* video = ref.info.primaryVideo()) {
        if (!video->codecName.empty()) {
            parts << QString::fromStdString(video->codecName);
        }
        parts << QString("%1×%2").arg(video->width).arg(video->height);
        if (const double rate = video->frameRate.toDouble(); rate > 0.0) {
            parts << QString("%1 fps").arg(rate, 0, 'g', 4);
        }
    } else if (const media::AudioStreamInfo* audio = ref.info.primaryAudio()) {
        if (!audio->codecName.empty()) {
            parts << QString::fromStdString(audio->codecName);
        }
        parts << QString("%1 kHz").arg(audio->sampleRate.toDouble() / 1000.0, 0, 'g', 4);
        parts << (audio->channelCount == 1   ? QStringLiteral("mono")
                  : audio->channelCount == 2 ? QStringLiteral("stereo")
                                             : QString("%1 ch").arg(audio->channelCount));
    }
    // Kept on the row rather than in a menu: a file being read as something
    // other than what it claims is exactly the kind of setting somebody forgets
    // they made, and the bin is where they would look for it.
    if (ref.transferOverride != media::TransferFunction::Unknown) {
        parts << QString("[%1]").arg(QString::fromUtf8(media::toString(ref.transferOverride)));
    }
    if (ref.primariesOverride != media::ColorPrimaries::Unknown) {
        parts << QString("[%1]").arg(QString::fromUtf8(media::toString(ref.primariesOverride)));
    }
    return parts.join(" · ");
}

QString describe(const model::Subclip& subclip, const model::MediaRef& source) {
    return QString::fromStdString(subclip.name.empty() ? source.name : subclip.name);
}

/// Bytes as the footer says them. One decimal, because the number is a sense of
/// scale and not an accounting.
QString humanSize(std::uintmax_t bytes) {
    constexpr double kUnit = 1024.0;
    const double value = static_cast<double>(bytes);
    if (value >= kUnit * kUnit * kUnit) {
        return QString("%1 GB").arg(value / (kUnit * kUnit * kUnit), 0, 'f', 1);
    }
    if (value >= kUnit * kUnit) {
        return QString("%1 MB").arg(value / (kUnit * kUnit), 0, 'f', 1);
    }
    return QString("%1 kB").arg(value / kUnit, 0, 'f', 0);
}

/// Every media id any sequence puts on a track.
///
/// The design marks used footage with a dot, and the honest answer to "is this
/// used" is a walk of the cut. Done once per refresh rather than once per row:
/// a bin of three hundred files against a cut of a thousand clips is otherwise
/// three hundred thousand comparisons for a five-pixel dot.
std::set<std::uint64_t> usedMedia(const model::Project& project) {
    std::set<std::uint64_t> used;
    for (const model::Sequence& sequence : project.sequences()) {
        for (const auto* tracks : {&sequence.videoTracks(), &sequence.audioTracks()}) {
            for (const model::Track& track : *tracks) {
                for (const model::Clip& clip : track.clips()) {
                    if (clip.source.isValid()) {
                        used.insert(clip.source.value());
                    }
                }
            }
        }
    }
    return used;
}

/// The rows, painted.
///
/// A delegate rather than a widget per row for the reason given on the class,
/// and because the design's row is a thumbnail, two lines of type and a dot --
/// four draw calls, against four widgets and a layout each.
class BinDelegate : public QStyledItemDelegate {
public:
    explicit BinDelegate(const bool* compact, QObject* parent = nullptr)
        : QStyledItemDelegate{parent}, compact_{compact} {}

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        Q_UNUSED(option);
        if (index.data(kRoleHeader).toBool()) {
            return QSize{0, kHeaderHeight};
        }
        const bool subclip = index.data(kRoleSubclip).toULongLong() != 0;
        return QSize{0, *compact_ || subclip ? kCompactRowHeight : kRowHeight};
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRect row = option.rect;

        if (index.data(kRoleHeader).toBool()) {
            paintHeader(painter, row, index);
            painter->restore();
            return;
        }

        // The row's own ground. Selection is the accent wash the rest of the
        // application uses for a picked thing; hover is the design's 6%.
        const QRect plate = row.adjusted(kGutter / 2, 1, -kGutter / 2, -1);
        if ((option.state & QStyle::State_Selected) != 0) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme::mix(theme::surface(), theme::accent(), 0.20));
            painter->drawRoundedRect(plate, 7, 7);
        } else if ((option.state & QStyle::State_MouseOver) != 0) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme::mix(theme::surface(), theme::text(), 0.06));
            painter->drawRoundedRect(plate, 7, 7);
        }

        const bool subclip = index.data(kRoleSubclip).toULongLong() != 0;
        int textLeft = plate.left() + 6;
        if (!*compact_ && !subclip) {
            const QRect thumb{plate.left() + 6, plate.top() + (plate.height() - kThumbHeight) / 2,
                              kThumbWidth, kThumbHeight};
            paintThumbnail(painter, thumb, index);
            textLeft = thumb.right() + 9;
        } else if (subclip) {
            // Indented under the file it is a note about, and marked with the
            // range rather than a picture: a subclip is somebody saying where
            // the good part is, not a second file.
            textLeft = plate.left() + 20;
        }

        int textRight = plate.right() - 8;
        if (index.data(kRoleUsed).toBool()) {
            const QRect dot{plate.right() - 11, plate.center().y() - 2, 5, 5};
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme::accent(500));
            painter->drawEllipse(dot);
            textRight = dot.left() - 6;
        }

        const QString meta = index.data(kRoleMeta).toString();
        const bool twoLine = !*compact_ && !subclip && !meta.isEmpty();
        const QRect text{textLeft, plate.top(), std::max(0, textRight - textLeft), plate.height()};

        QFont name = option.font;
        name.setPointSizeF(9.5);
        painter->setFont(name);
        painter->setPen(theme::text());
        const QFontMetrics nameMetrics{name};
        const QString shownName = nameMetrics.elidedText(index.data(Qt::DisplayRole).toString(),
                                                         Qt::ElideMiddle, text.width());
        if (twoLine) {
            painter->drawText(
                QRect{text.left(), text.top() + 5, text.width(), nameMetrics.height()},
                Qt::AlignLeft | Qt::AlignVCenter, shownName);
            QFont small = option.font;
            small.setPointSizeF(8.0);
            painter->setFont(small);
            painter->setPen(theme::textAt(0.45));
            const QFontMetrics smallMetrics{small};
            painter->drawText(QRect{text.left(), text.bottom() - smallMetrics.height() - 5,
                                    text.width(), smallMetrics.height()},
                              Qt::AlignLeft | Qt::AlignVCenter,
                              smallMetrics.elidedText(meta, Qt::ElideRight, text.width()));
        } else {
            painter->drawText(text, Qt::AlignLeft | Qt::AlignVCenter, shownName);
            // In one line the running time is the only other thing that fits,
            // and it is the one somebody is looking for.
            const QString badge = index.data(kRoleBadge).toString();
            if (!badge.isEmpty()) {
                QFont small = option.font;
                small.setPointSizeF(8.0);
                painter->setFont(small);
                painter->setPen(theme::textAt(0.45));
                painter->drawText(text, Qt::AlignRight | Qt::AlignVCenter, badge);
            }
        }
        painter->restore();
    }

private:
    void paintHeader(QPainter* painter, const QRect& row, const QModelIndex& index) const {
        const QColor ink = theme::textAt(0.48);
        const bool folded = index.data(kRoleFolded).toBool();
        const QPixmap caret =
            icons::pixmap(folded ? icons::Glyph::CaretRight : icons::Glyph::CaretDown, 11, ink);
        painter->drawPixmap(QPoint{row.left() + 6, row.center().y() - 5}, caret);

        QFont label = painter->font();
        label.setPointSizeF(8.0);
        label.setCapitalization(QFont::AllUppercase);
        // The design tracks its headings wide, which is what keeps a
        // three-letter folder name from reading as a typo.
        label.setLetterSpacing(QFont::PercentageSpacing, 109);
        painter->setFont(label);
        painter->setPen(ink);
        const QRect text{row.left() + 22, row.top(), row.width() - 52, row.height()};
        painter->drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
                          index.data(Qt::DisplayRole).toString());

        QFont tally = painter->font();
        tally.setCapitalization(QFont::MixedCase);
        tally.setLetterSpacing(QFont::PercentageSpacing, 100);
        painter->setFont(tally);
        painter->drawText(QRect{row.left(), row.top(), row.width() - 10, row.height()},
                          Qt::AlignRight | Qt::AlignVCenter, index.data(kRoleCount).toString());
    }

    /// The placeholder frame the design draws: a dark gradient plate, a hairline
    /// inside it, the file's kind in the middle, and the running time in the
    /// corner. No decoded frame -- pulling one is a seek per row, and the design
    /// itself shows a glyph.
    void paintThumbnail(QPainter* painter, const QRect& thumb, const QModelIndex& index) const {
        QPainterPath plate;
        plate.addRoundedRect(thumb, 5, 5);
        QLinearGradient wash{thumb.topLeft(), thumb.bottomRight()};
        wash.setColorAt(0.0, theme::neutral(800));
        wash.setColorAt(1.0, theme::neutral(900));
        painter->setPen(Qt::NoPen);
        painter->fillPath(plate, wash);

        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen{theme::mix(theme::neutral(900), theme::text(), 0.10), 1.0});
        painter->drawRoundedRect(QRectF{thumb}.adjusted(0.5, 0.5, -0.5, -0.5), 4.5, 4.5);

        const auto glyph = static_cast<icons::Glyph>(index.data(kRoleGlyph).toInt());
        const QPixmap picture =
            icons::pixmap(glyph, 15, theme::mix(theme::neutral(900), theme::text(), 0.30));
        painter->drawPixmap(QPoint{thumb.center().x() - 7, thumb.center().y() - 7}, picture);

        const QString badge = index.data(kRoleBadge).toString();
        if (badge.isEmpty()) {
            return;
        }
        QFont mono{QStringLiteral("Menlo")};
        mono.setStyleHint(QFont::Monospace);
        mono.setPointSizeF(7.0);
        painter->setFont(mono);
        const QFontMetrics metrics{mono};
        const int width = metrics.horizontalAdvance(badge) + 6;
        const QRect box{thumb.right() - 3 - width, thumb.bottom() - 2 - metrics.height(), width,
                        metrics.height()};
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor{0, 0, 0, 140});
        painter->drawRoundedRect(box, 3, 3);
        painter->setPen(theme::textAt(0.85));
        painter->drawText(box, Qt::AlignCenter, badge);
    }

    const bool* compact_;
};

/// A path in the one spelling the project stores.
///
/// The same file arrives spelled two ways: Qt hands out forward slashes, the
/// filesystem walk behind a dropped folder hands out backslashes, and Windows
/// treats either drive letter as the same disk. Without a single spelling, a
/// folder dropped after the files inside it imports every one of them a second
/// time.
QString canonicalPath(const QString& path) {
    return QFileInfo{path}.absoluteFilePath();
}

/// The same path as something two spellings of one file compare equal on.
QString pathKey(const QString& path) {
#ifdef Q_OS_WIN
    return canonicalPath(path).toLower();
#else
    return canonicalPath(path);
#endif
}

}  // namespace

ProjectBin::ProjectBin(QWidget* parent) : QWidget{parent} {
    setObjectName("bin-panel");
    // The panel paints its own surface, which a plain QWidget does not do for a
    // stylesheet background.
    setAttribute(Qt::WA_StyledBackground, true);

    // --- the tab strip ---------------------------------------------------
    //
    // Four tabs because the design has four. Only Media is a list of things
    // this panel owns; the other three name panels that live elsewhere in the
    // window, and say so rather than pretending to be empty.
    auto* tabBar = new QFrame(this);
    tabBar->setObjectName("bin-tabbar");
    tabBar->setFixedHeight(34);
    auto* tabRow = new QHBoxLayout(tabBar);
    tabRow->setContentsMargins(8, 0, 6, 0);
    tabRow->setSpacing(2);

    auto* tabs = new QButtonGroup(this);
    tabs->setExclusive(true);
    const QStringList tabNames{"Media", "Effects", "Titles", "Audio"};
    for (int index = 0; index < tabNames.size(); ++index) {
        auto* tab = new QPushButton(tabNames.at(index), tabBar);
        tab->setObjectName("bin-tab");
        tab->setCheckable(true);
        tab->setChecked(index == 0);
        tab->setCursor(Qt::PointingHandCursor);
        tabs->addButton(tab, index);
        tabRow->addWidget(tab);
    }
    tabRow->addStretch(1);

    auto* overflow = new QPushButton(tabBar);
    overflow->setObjectName("bin-tab");
    overflow->setIcon(icons::toolIcon(icons::Glyph::DotsThree, 14));
    overflow->setFixedSize(26, 24);
    overflow->setToolTip("What else this panel can do");
    tabRow->addWidget(overflow);
    connect(overflow, &QPushButton::clicked, this, [this] { overflowMenu(); });

    // --- search, and the view toggle beside it ---------------------------
    //
    // A filter rather than a second panel. A bin is looked *in*, and typing
    // three letters of a file name is how: at thirty clips the list is already
    // longer than the panel is tall.
    search_ = new QLineEdit(this);
    search_->setObjectName("bin-search");
    search_->setPlaceholderText("Search media");
    search_->setClearButtonEnabled(true);
    search_->setToolTip("Search name, codec, size, folder and notes");
    search_->addAction(QIcon{icons::pixmap(icons::Glyph::Magnifier, 13, theme::textAt(0.45))},
                       QLineEdit::LeadingPosition);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) {
        filter_ = text.trimmed();
        applyFilter();
    });

    compactButton_ = new QPushButton(this);
    compactButton_->setObjectName("bin-glyph-button");
    compactButton_->setIcon(icons::toolIcon(icons::Glyph::Rows, 14));
    compactButton_->setCheckable(true);
    compactButton_->setFixedSize(28, 28);
    compactButton_->setToolTip("List instead of thumbnails");
    connect(compactButton_, &QPushButton::toggled, this, [this](bool on) {
        compact_ = on;
        // Every row's height just changed, and a view only asks the delegate
        // again when it is told to lay out. Not a refresh: rebuilding the items
        // would drop the selection, and changing how a list is drawn should not
        // lose the place somebody was at in it.
        list_->doItemsLayout();
    });

    auto* searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(8, 8, 8, 0);
    searchRow->setSpacing(6);
    searchRow->addWidget(search_, 1);
    searchRow->addWidget(compactButton_);

    // --- the counted chips ------------------------------------------------
    chipHolder_ = new QWidget(this);
    auto* chipFlow = new chrome::FlowLayout{chipHolder_, 8, 5};
    chipHolder_->setLayout(chipFlow);
    // Without this the column above says "one row of chips" for ever and the
    // second row is drawn over the list.
    QSizePolicy chipPolicy{QSizePolicy::Preferred, QSizePolicy::Minimum};
    chipPolicy.setHeightForWidth(true);
    chipHolder_->setSizePolicy(chipPolicy);
    chipGroup_ = new QButtonGroup(this);
    chipGroup_->setExclusive(false);

    // Files dragged from the file manager land on the pane, not on whichever
    // child they happen to be over: the list is NoDragDrop, so its viewport
    // lets the event through, and the search field is told to do the same
    // rather than take a path in as text to search for.
    setAcceptDrops(true);
    search_->setAcceptDrops(false);

    // --- the list ---------------------------------------------------------
    list_ = new QListWidget(this);
    list_->setObjectName("bin-list");
    list_->setFrameShape(QFrame::NoFrame);
    list_->setMouseTracking(true);
    list_->setUniformItemSizes(false);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->setItemDelegate(new BinDelegate{&compact_, list_});
    list_->viewport()->setAutoFillBackground(false);

    footer_ = new QLabel(this);
    footer_->setObjectName("bin-footer");
    footer_->setFixedHeight(28);
    footer_->setContentsMargins(12, 0, 12, 0);

    // Media is the only tab with a list behind it; the rest are a sentence
    // pointing at the panel that does own them.
    pages_ = new QStackedWidget(this);
    pages_->addWidget(list_);
    for (const QString& elsewhere :
         {QStringLiteral("Effects live in the Effects panel, beside the monitor."),
          QStringLiteral("Titles are made on the timeline: add a title clip to a video track."),
          QStringLiteral("Audio levels and sends live in the Mixer, under the Audio workspace.")}) {
        auto* note = new QLabel(elsewhere, this);
        note->setWordWrap(true);
        note->setAlignment(Qt::AlignCenter);
        note->setContentsMargins(20, 0, 20, 0);
        note->setProperty("muted", true);
        pages_->addWidget(note);
    }
    connect(tabs, &QButtonGroup::idClicked, this, [this](int id) { pages_->setCurrentIndex(id); });

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(tabBar);
    layout->addLayout(searchRow);
    layout->addWidget(chipHolder_);
    layout->addWidget(pages_, 1);
    layout->addWidget(footer_);

    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (item != nullptr && !item->data(kRoleHeader).toBool()) {
            appendSelectedToTimeline();
        }
    });
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        // Headings fold. The caret says so, and a bin with four cards in it is
        // otherwise a list somebody scrolls past rather than reads.
        if (item == nullptr || !item->data(kRoleHeader).toBool()) {
            return;
        }
        const QString bin = item->data(kRoleBin).toString();
        if (collapsed_.contains(bin)) {
            collapsed_.remove(bin);
        } else {
            collapsed_.insert(bin);
        }
        item->setData(kRoleFolded, collapsed_.contains(bin));
        applyFilter();
    });
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
    connect(list_, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint& where) { rowMenu(where); });

    rebuildChips();
    applyFilter();
}

/// The chips: one per folder, counted, plus the two that are not folders.
///
/// Rebuilt from the project rather than kept in step by hand, because the set
/// of folders changes every time somebody imports and a chip for a folder with
/// nothing in it is a filter that shows an empty list.
void ProjectBin::rebuildChips() {
    QLayout* chips = chipHolder_->layout();
    while (QLayoutItem* old = chips->takeAt(0)) {
        if (QWidget* widget = old->widget()) {
            chipGroup_->removeButton(qobject_cast<QAbstractButton*>(widget));
            // Orphaned now and deleted later: this is reached from a chip's own
            // toggled(), so deleting outright would pull the ground from under
            // the signal that got here -- but leaving it parented would keep it
            // on screen, drawn over the chips that replace it, until the event
            // loop got round to the deletion.
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete old;
    }

    std::map<QString, int> counts;
    int total = 0;
    if (project_ != nullptr) {
        for (const model::MediaRef& ref : project_->media()) {
            ++counts[binNameFor(ref.path)];
            ++total;
        }
    }

    const auto addChip = [this, chips](const QString& label, bool on, bool outline,
                                       const std::function<void(bool)>& picked) {
        auto* chip = new QPushButton(label, this);
        chip->setObjectName("bin-chip");
        chip->setCheckable(true);
        chip->setChecked(on);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setProperty("outline", outline);
        chipGroup_->addButton(chip);
        chips->addWidget(chip);
        connect(chip, &QPushButton::toggled, this, picked);
        return chip;
    };

    addChip(QString("All %1").arg(total), bin_.isEmpty(), false, [this](bool on) {
        if (on) {
            setBinFilter({});
        }
    });
    for (const auto& [name, tally] : counts) {
        const QString bin = name;
        addChip(QString("%1 %2").arg(bin).arg(tally), bin_ == bin, false,
                [this, bin](bool on) { setBinFilter(on ? bin : QString{}); });
    }
    // The design's outline chip is Favourites. There is no favourite in this
    // project's model and inventing one would be a field to save, load and
    // migrate for the sake of a chip -- so it filters on the fact the row
    // already shows: whether the cut uses this file.
    auto* used = addChip(QStringLiteral("Used"), usedOnly_, true, [this](bool on) {
        usedOnly_ = on;
        applyFilter();
    });
    used->setToolTip("Only footage the cut actually uses");

    chipHolder_->updateGeometry();
}

/// Pick one folder, or all of them, and keep the chips agreeing about it.
void ProjectBin::setBinFilter(const QString& bin) {
    if (bin_ == bin) {
        return;
    }
    bin_ = bin;
    rebuildChips();
    applyFilter();
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
    int rows = 0;

    // A heading's visibility is decided by its rows, and its rows come after
    // it: matches are tallied per folder on the way down, then the headings
    // are set from the tally.
    std::map<QString, int> matchesPerBin;
    std::vector<QListWidgetItem*> headers;
    for (int row = 0; row < list_->count(); ++row) {
        QListWidgetItem* item = list_->item(row);
        const QString bin = item->data(kRoleBin).toString();
        if (item->data(kRoleHeader).toBool()) {
            headers.push_back(item);
            matchesPerBin.try_emplace(bin, 0);
            continue;
        }
        ++rows;

        const model::MediaRef* ref =
            project_ != nullptr
                ? project_->findMedia(model::MediaRefId{item->data(kRoleMedia).toULongLong()})
                : nullptr;
        const bool matchesText =
            filter_.isEmpty() ||
            (ref != nullptr ? model::matchesSearch(*ref, query)
                            : item->text().contains(filter_, Qt::CaseInsensitive));
        const bool matchesBin = bin_.isEmpty() || bin == bin_;
        const bool matchesUsed = !usedOnly_ || item->data(kRoleUsed).toBool();
        const bool matches = matchesText && matchesBin && matchesUsed;
        matchesPerBin[bin] += matches ? 1 : 0;
        // Folding hides the rows without their stopping to count: the tally on
        // the heading is what is behind the fold, so it has to keep counting.
        item->setHidden(!matches || collapsed_.contains(bin));
        shown += matches ? 1 : 0;
    }
    for (QListWidgetItem* header : headers) {
        const QString bin = header->data(kRoleBin).toString();
        header->setHidden(matchesPerBin[bin] == 0);
        header->setData(kRoleCount, QString::number(matchesPerBin[bin]));
    }

    footer_->setText(filter_.isEmpty() && bin_.isEmpty() && !usedOnly_
                         ? summary()
                         : QString("%1 of %2 shown").arg(shown).arg(rows));
}

/// How many files, what they weigh, and where the proxies stand.
///
/// The weight is read from disk rather than from the probe: a bit rate times a
/// duration is an estimate, and the question this line answers -- will this
/// project fit on the drive I am about to copy it to -- deserves the real
/// number.
QString ProjectBin::summary() const {
    if (project_ == nullptr) {
        return QStringLiteral("No project");
    }
    const auto& media = project_->media();
    std::uintmax_t bytes = 0;
    int proxied = 0;
    for (const model::MediaRef& ref : media) {
        std::error_code code;
        const std::uintmax_t size = std::filesystem::file_size(ref.path, code);
        if (!code) {
            bytes += size;
        }
        proxied += ref.proxyPath.empty() ? 0 : 1;
    }

    const int total = static_cast<int>(media.size());
    QString proxies;
    if (total == 0 || proxied == 0) {
        proxies = QStringLiteral("No proxies");
    } else if (proxied < total) {
        proxies = QString("Proxies %1 of %2").arg(proxied).arg(total);
    } else {
        proxies = project_->usingProxies() ? QStringLiteral("Proxies ready")
                                           : QStringLiteral("Proxies ready · off");
    }
    return QString("%1 %2 · %3 · %4")
        .arg(total)
        .arg(total == 1 ? "item" : "items", humanSize(bytes), proxies);
}

/// The actions that used to be four buttons under the list.
///
/// In a menu because the design gives this panel no button row: at 296 pixels
/// wide, four buttons clipped every label to two letters, and the same actions
/// are on the row's own right-click where somebody would reach for them.
void ProjectBin::overflowMenu() {
    QMenu menu;
    const Selection chosen = selection();
    const bool haveOne = chosen.media.isValid();

    QAction* importAction = menu.addAction("Import…");
    QAction* transcode = menu.addAction("Import and transcode to ProRes…");
    menu.addSeparator();
    QAction* append = menu.addAction("Append to timeline");
    append->setEnabled(haveOne);
    QAction* replace = menu.addAction("Replace selected clip with this");
    replace->setEnabled(haveOne);
    QAction* interpret = menu.addAction("Interpret footage…");
    interpret->setEnabled(haveOne);
    QAction* notes = menu.addAction("Notes…");
    notes->setEnabled(haveOne);

    QAction* picked = menu.exec(QCursor::pos());
    if (picked == importAction) {
        importFiles();
    } else if (picked == transcode) {
        importTranscodedDialog();
    } else if (picked == append) {
        appendSelectedToTimeline();
    } else if (picked == replace) {
        emit replaceRequested(chosen.media);
    } else if (picked == interpret) {
        interpretMenu();
    } else if (picked == notes) {
        editNotes();
    }
}

void ProjectBin::rowMenu(const QPoint& where) {
    QListWidgetItem* item = list_->itemAt(where);
    if (item == nullptr || item->data(kRoleHeader).toBool()) {
        overflowMenu();
        return;
    }
    list_->setCurrentItem(item);
    overflowMenu();
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

void ProjectBin::bind(const ui::SequenceBinding& binding) {
    project_ = binding.project;
    sequenceId_ = binding.sequence;
    commands_ = binding.commands;
    refresh();
}

void ProjectBin::refresh() {
    list_->clear();
    if (project_ == nullptr) {
        rebuildChips();
        applyFilter();
        return;
    }

    const std::set<std::uint64_t> used = usedMedia(*project_);

    // Grouped by the folder each file came from, in the order the folders sort
    // -- which is stable across imports, where "the order they were added" is
    // not, and a bin that reorders itself when somebody imports one clip is a
    // bin nobody can find anything in twice.
    std::map<QString, std::vector<const model::MediaRef*>> byBin;
    for (const model::MediaRef& ref : project_->media()) {
        byBin[binNameFor(ref.path)].push_back(&ref);
    }

    for (const auto& [bin, refs] : byBin) {
        auto* header = new QListWidgetItem(bin, list_);
        header->setData(kRoleHeader, true);
        header->setData(kRoleBin, bin);
        header->setData(kRoleCount, QString::number(refs.size()));
        header->setData(kRoleFolded, collapsed_.contains(bin));
        // Enabled so it can be clicked shut, but never the selection: a folder
        // is not a thing to open in the monitor.
        header->setFlags(Qt::ItemIsEnabled);

        for (const model::MediaRef* ref : refs) {
            auto* item = new QListWidgetItem(
                QString::fromStdString(ref->name.empty() ? ref->path : ref->name), list_);
            item->setData(kRoleMedia, QVariant::fromValue<qulonglong>(ref->id.value()));
            item->setData(kRoleSubclip, QVariant::fromValue<qulonglong>(0));
            item->setData(kRoleHeader, false);
            item->setData(kRoleBin, bin);
            item->setData(kRoleMeta, metaFor(*ref));
            item->setData(kRoleBadge, badgeFor(ref->info.duration));
            item->setData(kRoleGlyph, static_cast<int>(glyphFor(*ref)));
            item->setData(kRoleUsed, used.count(ref->id.value()) != 0);
            QString tip = QString::fromStdString(ref->path);
            if (!ref->notes.empty()) {
                tip += "\n" + QString::fromStdString(ref->notes);
            }
            item->setToolTip(tip);

            // Subclips of this file, under it. Grouped rather than listed
            // separately, because what somebody looks for is the take, and the
            // take is found by finding the file it is in.
            for (const model::Subclip& subclip : project_->subclips()) {
                if (subclip.source != ref->id) {
                    continue;
                }
                auto* child = new QListWidgetItem(describe(subclip, *ref), list_);
                child->setData(kRoleMedia, QVariant::fromValue<qulonglong>(ref->id.value()));
                child->setData(kRoleSubclip, QVariant::fromValue<qulonglong>(subclip.id.value()));
                child->setData(kRoleHeader, false);
                child->setData(kRoleBin, bin);
                child->setData(
                    kRoleBadge,
                    QString("%1s").arg(subclip.range.duration().toSecondsDouble(), 0, 'f', 2));
                child->setData(kRoleUsed, false);
            }
        }
    }

    rebuildChips();
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
        app::warn(this, "Import", QString::fromStdString(done.error().message()));
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
    static_cast<void>(importPaths(chosen));
}

int ProjectBin::importPaths(const QStringList& paths) {
    if (project_ == nullptr || commands_ == nullptr) {
        return 0;
    }

    // A folder stands for the media in it. One level down, not a walk of the
    // tree: a card's clips are in its folder, and a recursive import of a home
    // directory somebody let go of over the wrong pane is not recoverable in
    // one undo.
    QStringList files;
    for (const QString& path : paths) {
        const QFileInfo info{path};
        if (!info.isDir()) {
            files.push_back(canonicalPath(path));
            continue;
        }
        auto listed = io::listFolder(path.toStdString());
        if (!listed) {
            continue;
        }
        for (const io::FolderEntry& entry : *listed) {
            if (!entry.isFolder) {
                files.push_back(canonicalPath(QString::fromStdString(entry.path)));
            }
        }
    }

    // Probed on this thread rather than a background one. A probe reads a
    // header, not a stream -- it takes milliseconds -- and every background
    // thread added is another lifetime to get right, which is what caused the
    // abort-on-quit bug.
    int added = 0;
    for (const QString& path : files) {
        const std::string where = path.toStdString();

        // Already in the project? Importing it twice gives two entries
        // pointing at one file, which is two things to grade and relink -- and
        // dropping the same folder twice is an easy thing to do. Compared by
        // key rather than by string, because a project holds paths written by
        // older imports as well as this one.
        const QString key = pathKey(path);
        const bool known = std::any_of(project_->media().begin(), project_->media().end(),
                                       [&key](const model::MediaRef& ref) {
                                           return pathKey(QString::fromStdString(ref.path)) == key;
                                       });
        if (known) {
            continue;
        }

        auto probed = zaro::platform::ffmpeg::probe(where);
        if (!probed) {
            continue;
        }
        model::MediaRef ref;
        ref.path = where;
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
            ++added;
        }
    }
    commands_->breakMerge();
    refresh();
    emit edited();
    return added;
}

namespace {

/// The local files in a drag, if it carries any this pane would take.
///
/// By extension, like the browser lists by extension: the answer is needed
/// while the pointer is moving, and opening every file under the cursor to be
/// sure is not something that can happen at that speed. A folder counts,
/// because a folder of rushes is the usual thing to let go of here.
QStringList droppedMedia(const QMimeData* mime) {
    QStringList paths;
    if (mime == nullptr || !mime->hasUrls()) {
        return paths;
    }
    for (const QUrl& url : mime->urls()) {
        if (!url.isLocalFile()) {
            continue;  // a URL is not a file this program can open
        }
        const QString path = url.toLocalFile();
        const QFileInfo info{path};
        if (info.isDir() || io::looksLikeMedia(path.toStdString())) {
            paths.push_back(path);
        }
    }
    return paths;
}

/// The border that says the pane will take what is over it.
///
/// A child laid over the list rather than a paint in the pane's own
/// `paintEvent`: the pane's layout has no margins, so its children cover every
/// pixel a border would be drawn on, and whatever it painted would be painted
/// over.
class DropHint : public QWidget {
public:
    explicit DropHint(QWidget* parent) : QWidget{parent} {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
    }

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), theme::mix(theme::surface(), theme::accent(), 0.14));
        painter.setPen(QPen{theme::accent(), 2.0, Qt::DashLine});
        painter.drawRoundedRect(QRectF{rect()}.adjusted(3.0, 3.0, -3.0, -3.0), 6.0, 6.0);
    }
};

}  // namespace

void ProjectBin::dragEnterEvent(QDragEnterEvent* event) {
    if (project_ == nullptr || commands_ == nullptr || droppedMedia(event->mimeData()).isEmpty()) {
        event->ignore();
        return;
    }
    // Copy rather than move: the files stay where the shoot left them, and the
    // project holds a path to them.
    event->setDropAction(Qt::CopyAction);
    event->acceptProposedAction();
    dropHover_ = true;
    footer_->setText(QStringLiteral("Drop to import"));
    if (dropHint_ == nullptr) {
        dropHint_ = new DropHint{this};
    }
    dropHint_->setGeometry(pages_->geometry());
    dropHint_->raise();
    dropHint_->show();
}

void ProjectBin::dragMoveEvent(QDragMoveEvent* event) {
    if (dropHover_) {
        event->setDropAction(Qt::CopyAction);
        event->acceptProposedAction();
        return;
    }
    event->ignore();
}

void ProjectBin::dragLeaveEvent(QDragLeaveEvent* event) {
    dropHover_ = false;
    if (dropHint_ != nullptr) {
        dropHint_->hide();
    }
    applyFilter();  // puts the summary back over the "Drop to import"
    event->accept();
}

void ProjectBin::dropEvent(QDropEvent* event) {
    dropHover_ = false;
    if (dropHint_ != nullptr) {
        dropHint_->hide();
    }
    const QStringList paths = droppedMedia(event->mimeData());
    if (paths.isEmpty()) {
        applyFilter();
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const int added = importPaths(paths);
    QApplication::restoreOverrideCursor();

    // `importPaths` has already put the summary back; only the case worth
    // remarking on -- files that could not be read, or were here already --
    // says anything more.
    if (added == 0) {
        footer_->setText(QStringLiteral("Nothing to import"));
    }
}

ProjectBin::Selection ProjectBin::selection() const {
    QListWidgetItem* item = list_->currentItem();
    if (item == nullptr || item->data(kRoleHeader).toBool()) {
        return {};
    }
    return Selection{model::MediaRefId{item->data(kRoleMedia).toULongLong()},
                     model::SubclipId{item->data(kRoleSubclip).toULongLong()}};
}

/// Say what a file's curve and gamut really are.
///
/// In the bin because they are facts about the file, and the bin is the list of
/// files. Not a guess this program could make for somebody: a flat shot and a
/// log shot are the same picture, and the only thing that can tell them apart
/// is a person who knows what the camera was set to.
///
/// Two submenus under one entry, because they are one question asked twice --
/// "the container is wrong about this file, here is what it really is" -- and
/// somebody correcting a log curve is often correcting the gamut in the same
/// breath. Separate top-level items would be two places to look for one job.
void ProjectBin::interpretMenu() {
    const Selection chosen = selection();
    const model::MediaRef* ref = project_ != nullptr ? project_->findMedia(chosen.media) : nullptr;
    if (ref == nullptr) {
        return;
    }

    QMenu menu;
    std::map<QAction*, media::TransferFunction> curves;
    QMenu* curveMenu = menu.addMenu("Curve");
    for (const media::TransferFunction transfer : media::allTransferFunctions()) {
        QAction* action =
            curveMenu->addAction(transfer == media::TransferFunction::Unknown
                                     ? QString("As the file says (%1)")
                                           .arg(QString::fromUtf8(media::toString(ref->transfer())))
                                     : QString::fromUtf8(media::toString(transfer)));
        action->setCheckable(true);
        action->setChecked(ref->transferOverride == transfer);
        curves.emplace(action, transfer);
    }

    std::map<QAction*, media::ColorPrimaries> gamuts;
    QMenu* gamutMenu = menu.addMenu("Gamut");
    for (const media::ColorPrimaries primaries : media::allColorPrimaries()) {
        QAction* action = gamutMenu->addAction(
            primaries == media::ColorPrimaries::Unknown
                ? QString("As the file says (%1)")
                      .arg(QString::fromUtf8(media::toString(ref->primaries())))
                : QString::fromUtf8(media::toString(primaries)));
        action->setCheckable(true);
        action->setChecked(ref->primariesOverride == primaries);
        gamuts.emplace(action, primaries);
    }

    QAction* picked = menu.exec(QCursor::pos());
    if (picked == nullptr) {
        return;
    }
    const auto curve = curves.find(picked);
    const auto gamut = gamuts.find(picked);
    if (curve == curves.end() && gamut == gamuts.end()) {
        return;
    }
    for (model::MediaRef& media : project_->mediaMutable()) {
        if (media.id != chosen.media) {
            continue;
        }
        if (curve != curves.end()) {
            media.transferOverride = curve->second;
        } else {
            media.primariesOverride = gamut->second;
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
