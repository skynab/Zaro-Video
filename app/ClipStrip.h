#pragma once

#include <QWidget>
#include <vector>

#include "zaro/core/model/Project.h"
#include "zaro/ui/SequenceBinding.h"

namespace zaro::app {

/// Every shot in the cut, in order, with a mark on the ones that are graded.
///
/// A colourist works a reel shot by shot, not by scrubbing a timeline: the
/// question is "which is next" and "have I done this one", and both are
/// answered by a row of shots you can count. This is that row -- the same
/// clips the timeline holds, read straight from the sequence, with no state of
/// its own beyond which one is picked.
class ClipStrip : public QWidget, public ui::SequenceBound {
    Q_OBJECT

public:
    explicit ClipStrip(QWidget* parent = nullptr);

    void bind(const ui::SequenceBinding& binding) override;
    /// Re-read the sequence: clips were added, removed, or graded.
    void refresh();
    /// Follow a selection made somewhere else, without echoing it back.
    void setSelection(model::TrackId track, model::ClipId clip);

    /// How many shots are in the strip, and how many carry a grade. For the
    /// status line, and for a test that wants the count without the pixels.
    [[nodiscard]] int count() const { return static_cast<int>(shots_.size()); }
    [[nodiscard]] int gradedCount() const;

signals:
    /// A shot was picked. The window decides what that means -- moving the
    /// playhead, and pointing the parameter panels at it.
    void chosen(zaro::model::TrackId track, zaro::model::ClipId clip);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    [[nodiscard]] QSize sizeHint() const override;

private:
    struct Shot {
        model::TrackId track;
        model::ClipId clip;
        QString name;
        bool graded{false};
    };

    [[nodiscard]] QRect tileRect(std::size_t at) const;

    model::Project* project_{nullptr};
    model::SequenceId sequenceId_;
    model::TrackId track_;
    model::ClipId clip_;
    std::vector<Shot> shots_;
};

}  // namespace zaro::app
