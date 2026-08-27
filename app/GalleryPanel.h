#pragma once

#include <QImage>
#include <QString>
#include <QWidget>
#include <vector>

#include "zaro/core/time/RationalTime.h"

class QLabel;
class QListWidget;

namespace zaro::app {

/// Stills and looks: the left column of the Color workspace.
///
/// **A still here is a reference frame**, which is what makes the gallery worth
/// having rather than a scrapbook. Grabbing one records the moment it was taken
/// from; picking one points the monitor's split compare at that moment, so the
/// picture on screen is half the grade being worked on and half the frame it is
/// being judged against. That mechanism already exists -- see the window's
/// `setComparing` -- and this is a way of aiming it.
///
/// The stills are session state, deliberately. A grabbed still is a note about
/// what somebody is doing this afternoon; writing them into the project would
/// mean a project file that grows every time a colourist glances at something,
/// and a decision about where the pictures live on disk.
class GalleryPanel : public QWidget {
    Q_OBJECT

public:
    explicit GalleryPanel(QWidget* parent = nullptr);

    /// Keep a frame, taken at `at`, labelled `name`.
    void addStill(const QImage& frame, const time::RationalTime& at, const QString& name);
    [[nodiscard]] int stillCount() const;

    /// Fill the look list from a folder of .cube files.
    void showLutFolder(const QString& folder);
    [[nodiscard]] const QString& lutFolder() const noexcept { return folder_; }

signals:
    /// A still was picked: compare against this moment.
    void stillChosen(zaro::time::RationalTime at);
    /// Somebody asked for a still of what is on screen now.
    void grabRequested();
    /// A look was double-clicked: put this .cube on the selected clip.
    void lutChosen(const QString& path);

private:
    void chooseLutFolder();

    struct Still {
        QImage frame;
        time::RationalTime at;
        QString name;
    };

    std::vector<Still> stills_;
    QListWidget* grid_{nullptr};
    QListWidget* luts_{nullptr};
    QLabel* count_{nullptr};
    QString folder_;
};

}  // namespace zaro::app
