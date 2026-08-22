#pragma once

#include <QString>
#include <QWidget>

namespace zaro::app {

class ProgramMonitor;

/// The burn-in over the program monitor: what the frame is, where it is, and
/// the guides somebody has asked for.
///
/// A transparent widget rather than something the renderer draws, for the same
/// reason the mask handles are one: the picture that leaves this application is
/// the picture, and a monitor overlay that could reach an export is a flag
/// somebody eventually forgets to clear. It takes no mouse events, so the mask
/// editor above it still gets every click.
class ViewerOverlay : public QWidget {
    Q_OBJECT

public:
    /// The monitor is not owned and must outlive the overlay; it is asked where
    /// the picture is, so the burn-in sits on the frame rather than in the
    /// letterbox bars beside it.
    explicit ViewerOverlay(ProgramMonitor* monitor, QWidget* parent = nullptr);

    void setInfo(const QString& clipName, const QString& timecode, const QString& format,
                 const QString& level);
    /// Action-safe and title-safe rectangles, and the thirds.
    void setGuides(bool on);
    [[nodiscard]] bool guides() const noexcept { return guides_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    ProgramMonitor* monitor_{nullptr};
    QString clipName_;
    QString timecode_;
    QString format_;
    QString level_;
    bool guides_{false};
};

}  // namespace zaro::app
