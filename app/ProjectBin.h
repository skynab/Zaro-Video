#pragma once

#include <QWidget>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/model/Project.h"

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

signals:
    /// Media was imported, or a clip appended.
    void edited();
    /// Open this in the source monitor.
    void openRequested(zaro::model::MediaRefId media);
    /// Open this subclip in the source monitor, marked to its range.
    void openSubclipRequested(zaro::model::SubclipId subclip);
    /// Point the selected timeline clip at this media instead.
    ///
    /// The bin does not know what is selected on the timeline, and should not:
    /// it is a list of what the project has. The window owns the selection and
    /// does the work.
    void replaceRequested(zaro::model::MediaRefId media);

private:
    void importFiles();
    void appendSelectedToTimeline();
    /// What is selected, as either a media reference or a subclip.
    struct Selection {
        model::MediaRefId media;
        model::SubclipId subclip;
    };
    [[nodiscard]] Selection selection() const;

    model::Project* project_{nullptr};
    model::SequenceId sequenceId_;
    edit::CommandStack* commands_{nullptr};
    QListWidget* list_{nullptr};
    QPushButton* importButton_{nullptr};
};

}  // namespace zaro::app
