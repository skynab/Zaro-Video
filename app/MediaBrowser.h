#pragma once

#include <QDialog>
#include <string>
#include <vector>

#include "zaro/core/Error.h"
#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/model/Project.h"

class QLabel;
class QListWidget;
class QPushButton;

#include "zaro/ui/SequenceBinding.h"

namespace zaro::app {

/// Look through folders and take what you want.
///
/// A window rather than a docked panel: browsing a card is something somebody
/// does for a minute at the start of a day, and a permanent panel would spend
/// the rest of the day taking space from the timeline. It stays open while
/// importing, because taking three clips from three folders is one errand.
class MediaBrowser : public QDialog, public ui::SequenceBound {
    Q_OBJECT

public:
    explicit MediaBrowser(QWidget* parent = nullptr);

    void bind(const ui::SequenceBinding& binding) override;

    /// Show a folder's contents. Public because that is the whole interface:
    /// the dialog is one way of asking, and a test is another.
    [[nodiscard]] Status showFolder(const std::string& path);
    [[nodiscard]] const std::string& folder() const noexcept { return folder_; }

    /// Import whatever is selected. Returns how many were added.
    [[nodiscard]] Result<int> importSelected();

    /// For tests and for "Import All": select every file in the listing.
    void selectAllFiles();

signals:
    /// Media was imported, so the bin and anything counting it must refresh.
    void imported();

private:
    void openSelected();
    void goUp();

    model::Project* project_{nullptr};
    edit::CommandStack* commands_{nullptr};
    std::string folder_;
    QLabel* path_{nullptr};
    QListWidget* list_{nullptr};
    QPushButton* importButton_{nullptr};
    QLabel* footer_{nullptr};
};

}  // namespace zaro::app
