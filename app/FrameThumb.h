#pragma once

#include <QImage>
#include <QString>
#include <QWidget>

namespace zaro::app {

/// A small still of the programme, with what it is written across the bottom.
///
/// **Not a monitor.** The program monitor is a `QRhiWidget` that owns a GPU
/// device and draws the composited texture without it ever reaching system
/// memory; there is exactly one of those, and moving it between workspaces
/// would tear its device down and build it again on every switch. This is the
/// cheap alternative for the places that want to see what the playhead is on
/// without being the thing you grade against -- it is handed an already
/// composited frame, holds it at thumbnail size, and draws it.
///
/// The frame is pushed in rather than pulled: whoever composites for the scopes
/// is already paying for the frame, and a second render for a 260-pixel picture
/// would be the same cost twice.
class FrameThumb : public QWidget {
    Q_OBJECT

public:
    explicit FrameThumb(QWidget* parent = nullptr);

    /// A display-referred frame. Scaled on the way in, so the widget holds a
    /// thumbnail rather than a copy of the picture.
    void setFrame(const QImage& frame);
    void clearFrame();
    /// What is under the playhead, and where the playhead is.
    void setCaption(const QString& name, const QString& timecode);

    /// Whether anything has been given to it yet. Public so the window can skip
    /// compositing for a thumbnail nobody is looking at.
    [[nodiscard]] bool hasFrame() const noexcept { return !frame_.isNull(); }

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] int heightForWidth(int width) const override;
    [[nodiscard]] bool hasHeightForWidth() const override { return true; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    [[nodiscard]] QRect pictureRect() const;

    QImage frame_;
    QString name_;
    QString timecode_;
};

}  // namespace zaro::app
