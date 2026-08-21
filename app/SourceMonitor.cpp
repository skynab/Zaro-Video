#include "SourceMonitor.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <algorithm>

#include "zaro/core/time/Timecode.h"

namespace zaro::app {

SourceMonitor::SourceMonitor(QWidget* parent) : QWidget{parent} {
    title_ = new QLabel("No source", this);
    title_->setStyleSheet("font-weight: 600;");
    marks_ = new QLabel(this);
    monitor_ = new ProgramMonitor(this);
    monitor_->setMinimumHeight(160);
    scrubber_ = new QSlider(Qt::Horizontal, this);

    // Short labels with tooltips. These sit in a narrow panel, and a clipped
    // label reads as a bug -- which is exactly how it looked the first two
    // times a button went in here without checking the rendered width.
    auto* markIn = new QPushButton("In", this);
    auto* markOut = new QPushButton("Out", this);
    auto* insert = new QPushButton("Insert", this);
    auto* overwrite = new QPushButton("Over", this);
    auto* subclip = new QPushButton("Subclip", this);
    subclip->setObjectName("make-subclip");
    subclip->setToolTip("Keep the marked range in the bin");
    markIn->setToolTip("Mark in point (I)");
    markOut->setToolTip("Mark out point (O)");
    insert->setToolTip("Insert the marked range at the playhead, pushing what follows (,)");
    overwrite->setToolTip("Overwrite at the playhead (.)");
    for (QPushButton* button : {markIn, markOut, insert, overwrite, subclip}) {
        // Never smaller than the text it holds.
        button->setMinimumWidth(button->sizeHint().width());
    }

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(markIn);
    buttons->addWidget(markOut);
    buttons->addWidget(subclip);
    buttons->addStretch(1);
    buttons->addWidget(insert);
    buttons->addWidget(overwrite);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(title_);
    layout->addWidget(monitor_, 1);
    layout->addWidget(scrubber_);
    layout->addWidget(marks_);
    layout->addLayout(buttons);

    connect(scrubber_, &QSlider::valueChanged, this, [this](int value) {
        if (mediaId_.isValid()) {
            setPosition(time::RationalTime{value, sequence_.frameRate()});
        }
    });
    connect(markIn, &QPushButton::clicked, this, &SourceMonitor::markIn);
    connect(markOut, &QPushButton::clicked, this, &SourceMonitor::markOut);
    connect(insert, &QPushButton::clicked, this, [this] { emit insertRequested(); });
    connect(overwrite, &QPushButton::clicked, this, [this] { emit overwriteRequested(); });
    connect(subclip, &QPushButton::clicked, this, [this] { emit subclipRequested(); });

    refresh();
}

void SourceMonitor::setProvider(render::SourceFrameProvider* provider) {
    provider_ = provider;
    if (mediaId_.isValid()) {
        monitor_->setSource(&sequence_, provider_);
    }
}

void SourceMonitor::load(const model::MediaRef& media) {
    const media::VideoStreamInfo* video = media.info.primaryVideo();
    const time::Rational rate = video != nullptr ? video->frameRate : time::rates::fps25;

    // A sequence of one clip, at the media's own rate and size, so the source
    // is shown as it is rather than conformed to the timeline.
    sequence_ = model::Sequence{model::SequenceId{1}, media.name, rate};
    sequence_.setSize(video != nullptr ? video->width : 1920,
                      video != nullptr ? video->height : 1080);
    sequence_.addTrack(model::TrackId{1}, model::TrackKind::Video, "Source");

    const auto duration = time::RationalTime::fromSeconds(media.info.duration, rate);
    if (duration.frames() > 0) {
        model::Clip clip;
        clip.id = model::ClipId{1};
        clip.source = media.id;
        clip.name = media.name;
        clip.sourceRange = time::TimeRange{time::RationalTime{0, rate}, duration};
        clip.timelineRange = clip.sourceRange;
        sequence_.tracksMutable(model::TrackKind::Video).front().insert(clip);
    }

    mediaId_ = media.id;
    // Marks belong to the media they were made on.
    in_.reset();
    out_.reset();
    position_ = time::RationalTime{0, rate};

    scrubber_->setRange(0, static_cast<int>(std::max<std::int64_t>(0, duration.frames() - 1)));
    scrubber_->setValue(0);
    monitor_->setSource(&sequence_, provider_);
    monitor_->setPosition(position_);
    refresh();
}

void SourceMonitor::showFrame(const model::MediaRef& media, const time::RationalTime& at) {
    // Reloaded only when it is a different file. Somebody who matched back to
    // a shot they were already working on has not asked to lose their marks.
    if (media.id != mediaId_) {
        load(media);
    }
    const time::Rational rate = sequence_.frameRate();
    const time::RationalTime clamped{std::max<std::int64_t>(0, at.rescaledTo(rate).frames()), rate};
    scrubber_->setValue(static_cast<int>(clamped.frames()));
    setPosition(clamped);
}

void SourceMonitor::loadMarked(const model::MediaRef& media, const time::TimeRange& range) {
    load(media);
    const time::Rational rate = sequence_.frameRate();
    in_ = range.start().rescaledTo(rate);
    // Exclusive, which is how markOut stores it: the value is one past the last
    // frame shown, and markedRange hands back a half-open range from the pair.
    out_ = range.endExclusive().rescaledTo(rate);
    scrubber_->setValue(static_cast<int>(in_->frames()));
    setPosition(*in_);
}

void SourceMonitor::setPosition(const time::RationalTime& position) {
    position_ = position;
    monitor_->setPosition(position_);
    refresh();
}

void SourceMonitor::step(std::int64_t frames) {
    if (!mediaId_.isValid()) {
        return;
    }
    const std::int64_t last = sequence_.duration().frames() - 1;
    const std::int64_t wanted =
        std::clamp<std::int64_t>(position_.frames() + frames, 0, std::max<std::int64_t>(0, last));
    scrubber_->setValue(static_cast<int>(wanted));
}

void SourceMonitor::markIn() {
    if (!mediaId_.isValid()) {
        return;
    }
    in_ = position_;
    // An in point after the out point would describe a negative range, so the
    // out is dropped rather than silently reordered behind the user's back.
    if (out_ && *out_ <= *in_) {
        out_.reset();
    }
    refresh();
}

void SourceMonitor::markOut() {
    if (!mediaId_.isValid()) {
        return;
    }
    // The out point is inclusive to the eye and exclusive in the model: marking
    // out on a frame means that frame is included.
    out_ = position_ + time::RationalTime{1, sequence_.frameRate()};
    if (in_ && *out_ <= *in_) {
        in_.reset();
    }
    refresh();
}

void SourceMonitor::clearMarks() {
    in_.reset();
    out_.reset();
    refresh();
}

std::optional<time::TimeRange> SourceMonitor::markedRange() const {
    if (!mediaId_.isValid()) {
        return std::nullopt;
    }
    const time::Rational& rate = sequence_.frameRate();
    // Unmarked ends fall back to the whole media, so marking only an in point
    // means "from here to the end", which is how it is normally used.
    const time::RationalTime start = in_.value_or(time::RationalTime{0, rate});
    const time::RationalTime end = out_.value_or(sequence_.duration());
    if (end <= start) {
        return std::nullopt;
    }
    return time::TimeRange::fromStartEnd(start, end);
}

void SourceMonitor::refresh() {
    if (!mediaId_.isValid()) {
        title_->setText("No source");
        marks_->setText("Double-click something in the project to open it here.");
        return;
    }
    const time::Rational& rate = sequence_.frameRate();
    const bool dropFrame = time::supportsDropFrame(rate);
    const auto asText = [&](const time::RationalTime& t) {
        return QString::fromStdString(
            time::timecodeFromFrames(t.frames(), rate, dropFrame).toString());
    };

    title_->setText(QString::fromStdString(sequence_.name()) + "   " + asText(position_));

    QString marks;
    marks += "In " + (in_ ? asText(*in_) : QString("—"));
    marks += "    Out " + (out_ ? asText(*out_ - time::RationalTime{1, rate}) : QString("—"));
    if (const auto range = markedRange()) {
        marks += QString("    (%1 frames)").arg(range->duration().frames());
    }
    marks_->setText(marks);
}

}  // namespace zaro::app
