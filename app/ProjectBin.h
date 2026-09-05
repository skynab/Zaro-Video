#pragma once

#include <QSet>
#include <QString>
#include <QWidget>
#include <string>
#include <vector>

#include "zaro/core/Error.h"
#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/model/Project.h"

class QAbstractButton;
class QButtonGroup;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;

#include "zaro/ui/SequenceBinding.h"

namespace zaro::app {

class ThumbnailCache;

/// The media pane: what media the project knows about, and how to look through
/// it.
///
/// Drawn as the design draws it -- a tab strip, a search field, a row of
/// counted filter chips, then thumbnail rows under folder headings, and a
/// summary along the bottom. The rows are painted by a delegate rather than
/// composed from widgets: a bin holds hundreds of files, and hundreds of
/// widgets is hundreds of things for the layout engine to visit every time
/// somebody types a letter into the search field.
class ProjectBin : public QWidget, public ui::SequenceBound {
    Q_OBJECT

public:
    explicit ProjectBin(QWidget* parent = nullptr);

    void bind(const ui::SequenceBinding& binding) override;
    void refresh();

    /// Where the rows get their preview frames. Not owned; may be null, in
    /// which case a row draws the placeholder plate the design falls back to.
    /// Shared with the timeline, which wants the same frames of the same files.
    void setThumbnailCache(ThumbnailCache* cache);

    /// Ask for files. Public because Import is a File-menu item as well as an
    /// item in this panel's overflow menu, and both should be the same action.
    void importFiles();

    /// Import these paths, and return how many the project gained.
    ///
    /// The action behind both ways of asking -- the file dialog and a drop
    /// from the file manager -- and behind the test that checks either. A
    /// folder among them is listed rather than refused: dropping a card's
    /// folder is how somebody hands over a shoot.
    int importPaths(const QStringList& paths);

    /// Transcode files into an editing codec and import the results.
    ///
    /// Public without the dialogs in the way, for the same reason `setNotes`
    /// is: it is the action, and the dialog is one way of asking for it.
    [[nodiscard]] Status importTranscoded(const std::vector<std::string>& paths,
                                          const std::string& destination,
                                          const std::string& videoCodec);

    /// How many media references the project holds, for the status bar.
    [[nodiscard]] int count() const;

    /// The line along the bottom of the pane: how many files, how much they
    /// weigh, and where the proxies stand. Public so the self-test can read it
    /// without scraping a label.
    [[nodiscard]] QString summary() const;

protected:
    // A drop from the file manager is the shortest path from "these are my
    // rushes" to "the project has them", so the pane takes files as well as
    // asking for them.
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

signals:
    /// Media was imported, or a clip appended.
    void edited();
    /// Files were added to the project.
    ///
    /// Separate from `edited`, which also covers appending a clip to the cut:
    /// this one means the *set of media* changed, and a media source opened
    /// before it resolved every file the project had at that moment. Nothing
    /// can decode a file imported since until it is opened again -- which is
    /// what the window does with this.
    void mediaImported();
    /// Open this in the source monitor.
    void openRequested(zaro::model::MediaRefId media);
    /// Open this subclip in the source monitor, marked to its range.
    void openSubclipRequested(zaro::model::SubclipId subclip);
    /// The footage's curve was corrected, so anything showing it must redraw
    /// and the media has to be reopened.
    void colorChanged();
    /// Somebody asked for a title from the Titles tab.
    ///
    /// The pane does not make one: a title is a clip on a sequence, and what
    /// the pane knows about is the project's media. The window owns the
    /// decision and the command.
    ///
    /// `presetId` is the row they asked for -- one of `titlePresets()`. It is
    /// carried because the pane lists three different graphics and used to ask
    /// for the same one whichever was double-clicked.
    void addTitleRequested(const QString& presetId);

    /// Point the selected timeline clip at this media instead.
    ///
    /// The bin does not know what is selected on the timeline, and should not:
    /// it is a list of what the project has. The window owns the selection and
    /// does the work.
    void replaceRequested(zaro::model::MediaRefId media);

private:
    void appendSelectedToTimeline();
    void editNotes();
    void importTranscodedDialog();

public:
    /// Write a note on a file. Public because it is the same action a script
    /// or the self-test performs, without the dialog in the way.
    void setNotes(model::MediaRefId media, const std::string& notes);

private:
    void applyFilter();
    void rebuildChips();
    void overflowMenu();
    void rowMenu(const QPoint& where);
    void setBinFilter(const QString& bin);

    /// What is selected, as either a media reference or a subclip.
    struct Selection {
        model::MediaRefId media;
        model::SubclipId subclip;
    };
    [[nodiscard]] Selection selection() const;
    void interpretMenu();

    model::Project* project_{nullptr};
    model::SequenceId sequenceId_;
    edit::CommandStack* commands_{nullptr};

    ThumbnailCache* thumbnails_{nullptr};
    QListWidget* list_{nullptr};
    /// The Titles tab's presets. Draggable; it holds no project state.
    QListWidget* titleList_{nullptr};
    QLineEdit* search_{nullptr};
    QLabel* footer_{nullptr};
    QWidget* chipHolder_{nullptr};
    QButtonGroup* chipGroup_{nullptr};
    QPushButton* compactButton_{nullptr};
    QStackedWidget* pages_{nullptr};
    /// The Media tab: the list, or a sentence when there is nothing in it.
    ///
    /// Every other tab says what it is for when it is empty; the one somebody
    /// lands in first showed a blank rectangle, on a pane that takes dropped
    /// files without ever saying so.
    QStackedWidget* mediaPage_{nullptr};
    QLabel* binEmpty_{nullptr};

    QString filter_;
    /// Which folder chip is picked; empty means all of them.
    QString bin_;
    /// Folder headings somebody has folded shut.
    QSet<QString> collapsed_;
    /// The outline chip: only what the cut actually uses.
    bool usedOnly_{false};
    bool compact_{false};
    /// Files are hovering over the pane, so it says it will take them.
    bool dropHover_{false};
    /// The border drawn over the list while they hover.
    QWidget* dropHint_{nullptr};
};

}  // namespace zaro::app
