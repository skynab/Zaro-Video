#include "StemsPanel.h"

#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

#include "Theme.h"

namespace zaro::app {
namespace {

constexpr int kHeaderHeight = 30;
constexpr int kRowHeight = 30;
constexpr int kMargin = 6;

/// A colour per role, from the design's stem list. Fixed rather than derived:
/// dialogue is the same blue every time it is drawn, which is what lets
/// somebody read the list without reading it.
QColor inkFor(model::AudioRole role) {
    switch (role) {
        case model::AudioRole::Dialogue:
            return QColor{0x8f, 0xc7, 0xd9};
        case model::AudioRole::Music:
            return QColor{0xc7, 0x8f, 0xd9};
        case model::AudioRole::Effects:
            return QColor{0x8f, 0xd9, 0xa8};
        case model::AudioRole::Ambience:
            return QColor{0x6a, 0xd9, 0xc7};
        case model::AudioRole::Unassigned:
            break;
    }
    return QColor{0x75, 0x79, 0x8c};
}

/// Minutes and seconds. A stem's running time is a rough measure of how much of
/// the programme it covers, and seconds past the minute is as fine as that
/// question needs answering.
QString lengthOf(double seconds) {
    const int total = static_cast<int>(std::lround(seconds));
    return QString("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QChar('0'));
}

}  // namespace

StemsPanel::StemsPanel(QWidget* parent) : QWidget{parent} {
    setObjectName("stems-panel");
    setAttribute(Qt::WA_StyledBackground, true);
    setCursor(Qt::PointingHandCursor);
    refresh();
}

QSize StemsPanel::sizeHint() const {
    return QSize{262,
                 kHeaderHeight + (static_cast<int>(stems_.size()) * kRowHeight) + (kMargin * 2)};
}

void StemsPanel::bind(const ui::SequenceBinding& binding) {
    project_ = binding.project;
    sequenceId_ = binding.sequence;
    refresh();
}

int StemsPanel::clipsIn(model::AudioRole role) const {
    const auto found = std::find_if(stems_.begin(), stems_.end(),
                                    [role](const Stem& stem) { return stem.role == role; });
    return found == stems_.end() ? 0 : found->clips;
}

void StemsPanel::refresh() {
    stems_.clear();
    const model::Sequence* sequence =
        project_ != nullptr ? project_->findSequence(sequenceId_) : nullptr;

    for (const model::AudioRole role : model::allAudioRoles()) {
        Stem stem;
        stem.role = role;
        // The model spells these lowercase, because that is how they are
        // written into a project file. A list reads as names.
        stem.name = QString::fromUtf8(model::toString(role));
        if (!stem.name.isEmpty()) {
            stem.name[0] = stem.name[0].toUpper();
        }
        stem.ink = inkFor(role);
        stems_.push_back(std::move(stem));
    }

    if (sequence != nullptr) {
        // Audio tracks only, because those are what the mix is made of -- see
        // `render::AudioGraph`, which sums exactly these. A role on a video
        // clip is a note about material that is not in the mix.
        for (const model::Track& track : sequence->audioTracks()) {
            for (const model::Clip& clip : track.clips()) {
                auto found = std::find_if(stems_.begin(), stems_.end(), [&clip](const Stem& stem) {
                    return stem.role == clip.role;
                });
                if (found == stems_.end()) {
                    continue;
                }
                ++found->clips;
                found->seconds += clip.timelineRange.duration().toSecondsDouble();
                // The earliest one, so picking a stem goes to its start rather
                // than to whichever clip happened to be visited first.
                if (!found->clip.isValid() || clip.timelineRange.start() < found->at) {
                    found->track = track.id();
                    found->clip = clip.id;
                    found->at = clip.timelineRange.start();
                }
            }
        }
    }

    // An empty Unassigned row is the good case and says nothing worth a line;
    // an empty Dialogue row is a fact about the cut, so the four real roles
    // stay whether or not anything carries them.
    std::erase_if(stems_, [](const Stem& stem) {
        return stem.role == model::AudioRole::Unassigned && stem.clips == 0;
    });

    if (picked_ >= static_cast<int>(stems_.size())) {
        picked_ = -1;
    }
    setMinimumHeight(sizeHint().height());
    updateGeometry();
    update();
}

QRect StemsPanel::rowRect(std::size_t at) const {
    return QRect{kMargin, kHeaderHeight + (static_cast<int>(at) * kRowHeight),
                 width() - (kMargin * 2), kRowHeight - 2};
}

void StemsPanel::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);

    QFont heading = font();
    heading.setPointSizeF(8.5);
    painter.setFont(heading);
    painter.setPen(theme::text());
    painter.drawText(QRect{12, 0, width() - 24, kHeaderHeight}, Qt::AlignLeft | Qt::AlignVCenter,
                     "Stems");
    painter.setPen(QPen{theme::divider(), 1.0});
    painter.drawLine(0, kHeaderHeight - 1, width(), kHeaderHeight - 1);

    QFont label = font();
    label.setPointSizeF(8.0);
    QFont mono{QStringLiteral("Menlo")};
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSizeF(7.5);

    for (std::size_t at = 0; at < stems_.size(); ++at) {
        const Stem& stem = stems_[at];
        const QRect row = rowRect(at);
        const bool empty = stem.clips == 0;

        if (static_cast<int>(at) == picked_) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme::mix(theme::surface(), theme::accent(), 0.14));
            painter.drawRoundedRect(row, 5, 5);
        }

        // The colour bar, dimmed when nothing carries the role: an empty stem
        // is still worth listing and should not look like a full one.
        painter.setPen(Qt::NoPen);
        QColor bar = stem.ink;
        if (empty) {
            bar.setAlpha(70);
        }
        painter.setBrush(bar);
        painter.drawRoundedRect(QRect{row.left() + 6, row.center().y() - 9, 3, 18}, 2, 2);

        painter.setFont(label);
        painter.setPen(empty ? theme::textAt(0.38) : theme::textAt(0.85));
        painter.drawText(QRect{row.left() + 18, row.top(), row.width() - 100, row.height()},
                         Qt::AlignLeft | Qt::AlignVCenter, stem.name);

        painter.setFont(mono);
        painter.setPen(theme::textAt(empty ? 0.28 : 0.45));
        painter.drawText(QRect{row.left(), row.top(), row.width() - 10, row.height()},
                         Qt::AlignRight | Qt::AlignVCenter,
                         empty ? QStringLiteral("—")
                               : QString("%1 · %2").arg(stem.clips).arg(lengthOf(stem.seconds)));
    }
}

void StemsPanel::mousePressEvent(QMouseEvent* event) {
    for (std::size_t at = 0; at < stems_.size(); ++at) {
        if (!rowRect(at).contains(event->pos())) {
            continue;
        }
        picked_ = static_cast<int>(at);
        update();
        // A stem with nothing in it has nowhere to go.
        if (stems_[at].clip.isValid()) {
            emit stemChosen(stems_[at].track, stems_[at].clip, stems_[at].at);
        }
        return;
    }
}

}  // namespace zaro::app
