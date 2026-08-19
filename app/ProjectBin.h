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

private:
    void importFiles();
    void appendSelectedToTimeline();

    model::Project* project_{nullptr};
    model::SequenceId sequenceId_;
    edit::CommandStack* commands_{nullptr};
    QListWidget* list_{nullptr};
    QPushButton* importButton_{nullptr};
};

}  // namespace zaro::app
