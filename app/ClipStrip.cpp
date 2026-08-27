#include "ClipStrip.h"

#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>

#include "Icons.h"
#include "Theme.h"

namespace zaro::app {
namespace {

// The design's tile: 96 wide, a 40-tall frame, the name under it.
constexpr int kTileWidth = 96;
constexpr int kTileGap = 6;
constexpr int kFrameHeight = 40;
constexpr int kNameHeight = 14;
constexpr int kMargin = 12;

/// A stand-in frame for a shot, from its name.
///
/// The design draws each tile as a tinted gradient rather than a decoded
/// picture, and so does this: a strip of forty shots is forty seeks, and the
/// tile's job is to be countable and tellable-apart, which a stable colour per
/// name does. The hue comes from the name, so the same shot is the same tile
/// every time the strip is drawn.
QLinearGradient tintFor(const QString& name, const QRect& box) {
    const int hue = static_cast<int>(qHash(name) % 360);
    QLinearGradient wash{box.topLeft(), box.bottomRight()};
    wash.setColorAt(0.0, QColor::fromHsl(hue, 70, 78));
    wash.setColorAt(1.0, QColor::fromHsl((hue + 24) % 360, 60, 44));
    return wash;
}

}  // namespace

ClipStrip::ClipStrip(QWidget* parent) : QWidget{parent} {
    setObjectName("clip-strip");
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(74);
    setCursor(Qt::PointingHandCursor);
}

QSize ClipStrip::sizeHint() const {
    return QSize{(static_cast<int>(shots_.size()) * (kTileWidth + kTileGap)) + (kMargin * 2), 74};
}

void ClipStrip::setProject(model::Project* project, model::SequenceId sequence) {
    project_ = project;
    sequenceId_ = sequence;
    refresh();
}

void ClipStrip::setSelection(model::TrackId track, model::ClipId clip) {
    track_ = track;
    clip_ = clip;
    update();
}

void ClipStrip::refresh() {
    shots_.clear();
    const model::Sequence* sequence =
        project_ != nullptr ? project_->findSequence(sequenceId_) : nullptr;
    if (sequence != nullptr) {
        // Video tracks only, and in timeline order. A colourist grades pictures;
        // a sound clip in the strip would be a tile that cannot be graded and
        // one more thing to skip past.
        for (const model::Track& track : sequence->videoTracks()) {
            for (const model::Clip& clip : track.clips()) {
                Shot shot;
                shot.track = track.id();
                shot.clip = clip.id;
                shot.name = QString::fromStdString(clip.name);
                // Graded means anything has been decided about this shot's
                // colour -- a wheel moved, a correction typed, a look put on
                // it. The mark answers "have I been here", so it has to catch
                // all three rather than only the one this panel edits.
                shot.graded = !clip.wheels.isIdentity() || !clip.color.isIdentity() ||
                              !clip.lut.path.empty();
                shots_.push_back(std::move(shot));
            }
        }
        std::stable_sort(shots_.begin(), shots_.end(), [sequence](const Shot& a, const Shot& b) {
            const model::Track* trackA = sequence->findTrack(a.track);
            const model::Track* trackB = sequence->findTrack(b.track);
            const model::Clip* clipA = trackA != nullptr ? trackA->find(a.clip) : nullptr;
            const model::Clip* clipB = trackB != nullptr ? trackB->find(b.clip) : nullptr;
            if (clipA == nullptr || clipB == nullptr) {
                return false;
            }
            return clipA->timelineRange.start() < clipB->timelineRange.start();
        });
    }
    updateGeometry();
    update();
}

int ClipStrip::gradedCount() const {
    return static_cast<int>(
        std::count_if(shots_.begin(), shots_.end(), [](const Shot& shot) { return shot.graded; }));
}

QRect ClipStrip::tileRect(std::size_t at) const {
    const int left = kMargin + (static_cast<int>(at) * (kTileWidth + kTileGap));
    return QRect{left, (height() - (kFrameHeight + kNameHeight)) / 2, kTileWidth,
                 kFrameHeight + kNameHeight};
}

void ClipStrip::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (shots_.empty()) {
        QFont note = font();
        note.setPointSizeF(9.0);
        painter.setFont(note);
        painter.setPen(theme::textAt(0.38));
        painter.drawText(rect(), Qt::AlignCenter, "No shots in this sequence yet");
        return;
    }

    QFont number{QStringLiteral("Menlo")};
    number.setStyleHint(QFont::Monospace);
    number.setPointSizeF(6.5);
    QFont name = font();
    name.setPointSizeF(7.0);

    for (std::size_t at = 0; at < shots_.size(); ++at) {
        const Shot& shot = shots_[at];
        const QRect tile = tileRect(at);
        const QRect frame{tile.left(), tile.top(), tile.width(), kFrameHeight};
        const bool picked = shot.track == track_ && shot.clip == clip_;

        painter.setPen(Qt::NoPen);
        painter.setBrush(tintFor(shot.name, frame));
        painter.drawRoundedRect(frame, 4, 4);

        painter.setBrush(Qt::NoBrush);
        painter.setPen(picked ? QPen{theme::accent(300), 1.5}
                              : QPen{theme::mix(theme::bg(), theme::text(), 0.12), 1.0});
        painter.drawRoundedRect(QRectF{frame}.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);

        painter.setFont(number);
        painter.setPen(QColor{255, 255, 255, 190});
        painter.drawText(frame.adjusted(4, 3, -4, 0), Qt::AlignLeft | Qt::AlignTop,
                         QString("%1").arg(at + 1, 2, 10, QChar('0')));

        const QPixmap mark = icons::pixmap(
            shot.graded ? icons::Glyph::CircleHalf : icons::Glyph::CircleDashed, 10,
            shot.graded ? theme::accent(200) : QColor{255, 255, 255, 110});
        painter.drawPixmap(QPoint{frame.right() - 13, frame.bottom() - 13}, mark);

        painter.setFont(name);
        painter.setPen(picked ? theme::textAt(0.8) : theme::textAt(0.45));
        const QFontMetrics metrics{name};
        painter.drawText(QRect{tile.left(), frame.bottom() + 3, tile.width(), kNameHeight},
                         Qt::AlignLeft | Qt::AlignVCenter,
                         metrics.elidedText(shot.name, Qt::ElideMiddle, tile.width()));
    }
}

void ClipStrip::mousePressEvent(QMouseEvent* event) {
    for (std::size_t at = 0; at < shots_.size(); ++at) {
        if (tileRect(at).contains(event->pos())) {
            emit chosen(shots_[at].track, shots_[at].clip);
            return;
        }
    }
}

}  // namespace zaro::app
