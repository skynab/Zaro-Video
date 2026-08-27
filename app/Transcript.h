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

/// The sequence's transcript, as something to edit by.
///
/// Deleting lines here deletes the picture and the sound under them: this is
/// what "editing by deleting words" is, and the whole of it is that the
/// transcript is the selection and the timeline follows.
///
/// A window rather than a docked panel, like the media browser: reading a
/// transcript is something somebody does while looking at it and not while
/// trimming, and it wants width rather than a column beside the timeline.
class Transcript : public QDialog, public ui::SequenceBound {
    Q_OBJECT

public:
    explicit Transcript(QWidget* parent = nullptr);

    void bind(const ui::SequenceBinding& binding) override;
    void refresh();

    /// Delete whatever is selected, from the transcript and from the sequence.
    /// Returns how many lines went.
    [[nodiscard]] Result<int> deleteSelected();

    /// Select the lines containing any of these words, so filler removal and a
    /// search are the same gesture underneath.
    int selectContaining(const std::vector<std::string>& words);

    [[nodiscard]] int lineCount() const;

signals:
    /// The sequence changed, so everything showing it needs redrawing.
    void edited();
    /// Somebody clicked a line: put the playhead there.
    void scrubbed(zaro::time::RationalTime at);

private:
    model::Project* project_{nullptr};
    model::SequenceId sequenceId_;
    edit::CommandStack* commands_{nullptr};
    QListWidget* list_{nullptr};
    QLabel* footer_{nullptr};
};

}  // namespace zaro::app
