#pragma once

#include <QWidget>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/model/Project.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace zaro::app {

/// The project bin: what media the project knows about, and how to add more.
class ProjectBin : public QWidget {
    Q_OBJECT

public:
    explicit ProjectBin(QWidget* parent = nullptr);

    void setProject(model::Project* project, model::SequenceId sequence,
                    edit::CommandStack* commands);
    void refresh();

    /// Ask for files. Public because Import is a File-menu item as well as a
    /// button in this panel, and both should be the same action.
    void importFiles();

    /// How many media references the project holds, for the status bar.
    [[nodiscard]] int count() const;

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
    void applyFilter();
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
    QPushButton* importButton_{nullptr};
    QLineEdit* search_{nullptr};
    QLabel* footer_{nullptr};
    QString filter_;
};

}  // namespace zaro::app
