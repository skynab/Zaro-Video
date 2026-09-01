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
    /// Open this in the source monitor.
    void openRequested(zaro::model::MediaRefId media);
    /// Open this subclip in the source monitor, marked to its range.
    void openSubclipRequested(zaro::model::SubclipId subclip);
    /// The footage's curve was corrected, so anything showing it must redraw
    /// and the media has to be reopened.
    void colorChanged();
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

    QListWidget* list_{nullptr};
    QLineEdit* search_{nullptr};
    QLabel* footer_{nullptr};
    QWidget* chipHolder_{nullptr};
    QButtonGroup* chipGroup_{nullptr};
    QPushButton* compactButton_{nullptr};
    QStackedWidget* pages_{nullptr};

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
