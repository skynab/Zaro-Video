#pragma once

#include <QString>
#include <QWidget>
#include <vector>

#include "zaro/core/model/AudioRole.h"
#include "zaro/core/model/Project.h"
#include "zaro/core/time/RationalTime.h"

namespace zaro::app {

/// The stems: what the mix is made of, grouped by what each sound is for.
///
/// **A stem here is a role, not a bus.** The design lists named submixes --
/// Dialogue, Music, Effects, Ambience, and the deliverable stems beside them --
/// and this project has no bus to point those at. What it does have is
/// `model::AudioRole` on every clip, and the four roles it defines are the same
/// four the design names, for the same reason: they are what a mix is built out
/// of. So a stem is the set of clips carrying one role, and this is a list of
/// those sets.
///
/// That makes the panel answer the question a stem list is really asked during
/// an audio pass -- has this been sorted out yet, and where is the music -- and
/// it stays true without a submix model behind it.
class StemsPanel : public QWidget {
    Q_OBJECT

public:
    explicit StemsPanel(QWidget* parent = nullptr);

    void setProject(model::Project* project, model::SequenceId sequence);
    /// Re-read the sequence: clips were added, removed, or re-tagged.
    void refresh();

    /// How many clips carry a role. For the status line and for a test that
    /// wants the tally without the pixels.
    [[nodiscard]] int clipsIn(model::AudioRole role) const;

signals:
    /// A stem was picked: go and look at it. Carries the first clip of that
    /// role, so the window can move the playhead and select it.
    void stemChosen(zaro::model::TrackId track, zaro::model::ClipId clip,
                    zaro::time::RationalTime at);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    [[nodiscard]] QSize sizeHint() const override;

private:
    struct Stem {
        model::AudioRole role{};
        QString name;
        QColor ink;
        int clips{0};
        double seconds{0.0};
        /// Where the first clip of this role starts, for going to it.
        model::TrackId track;
        model::ClipId clip;
        time::RationalTime at;
    };

    [[nodiscard]] QRect rowRect(std::size_t at) const;

    model::Project* project_{nullptr};
    model::SequenceId sequenceId_;
    std::vector<Stem> stems_;
    int picked_{-1};
};

}  // namespace zaro::app
