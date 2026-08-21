#pragma once

#include <QWidget>
#include <optional>

#include "zaro/core/model/Project.h"
#include "zaro/core/render/FrameSource.h"

#include "ProgramMonitor.h"

class QLabel;
class QSlider;

namespace zaro::app {

/// The source monitor: one piece of media, with in and out points marked on it.
///
/// It is a sequence with a single clip in it. That is not a shortcut — it is
/// what a source monitor *is* — and it means the program monitor renders both,
/// rather than there being a second rendering path to keep in agreement with
/// the first.
class SourceMonitor : public QWidget {
    Q_OBJECT

public:
    explicit SourceMonitor(QWidget* parent = nullptr);

    /// Neither is owned. The provider resolves media the project already knows.
    void setProvider(render::SourceFrameProvider* provider);

    /// Show a media reference. Clears any marks: they belonged to the last one.
    void load(const model::MediaRef& media);

    /// Show a media reference at a particular frame of it, with no marks.
    ///
    /// What match frame needs: the answer to "which frame of the file is this"
    /// is `Clip::activeSourceTimeAt`, and this is where it is shown. There is
    /// no second implementation of that mapping, which is what keeps match
    /// frame right through trims, speed, reverse and time remapping.
    void showFrame(const model::MediaRef& media, const time::RationalTime& at);

    /// Show a media reference with a range already marked, for opening a
    /// subclip.
    void loadMarked(const model::MediaRef& media, const time::TimeRange& range);

    [[nodiscard]] model::MediaRefId media() const noexcept { return mediaId_; }
    /// Which frame of it is showing, in the media's own rate.
    [[nodiscard]] const time::RationalTime& position() const noexcept { return position_; }

    /// The marked range, in the media's own frame rate. Unmarked ends default
    /// to the start and end of the media, which is what makes marking only an
    /// in point useful.
    [[nodiscard]] std::optional<time::TimeRange> markedRange() const;

    void markIn();
    void markOut();
    void clearMarks();
    void step(std::int64_t frames);

signals:
    /// The user asked for the marked range to go onto the timeline.
    void insertRequested();
    void overwriteRequested();
    /// The user asked to keep the marked range as a subclip.
    ///
    /// The monitor does not make one: subclips live in the project, and the
    /// window owns that. What the monitor knows is what is marked.
    void subclipRequested();

private:
    void setPosition(const time::RationalTime& position);
    void refresh();

    ProgramMonitor* monitor_{nullptr};
    QLabel* title_{nullptr};
    QLabel* marks_{nullptr};
    QSlider* scrubber_{nullptr};

    render::SourceFrameProvider* provider_{nullptr};
    /// One clip, spanning the whole file, at the media's own rate and size.
    model::Sequence sequence_;
    model::MediaRefId mediaId_;
    time::RationalTime position_{};
    std::optional<time::RationalTime> in_;
    std::optional<time::RationalTime> out_;
};

}  // namespace zaro::app
