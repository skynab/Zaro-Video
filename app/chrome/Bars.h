// The window's chrome: the bars round the edges, and what they say.
//
// Two dozen labels, buttons and sliders that the window used to hold as two
// dozen members. Nothing owns them -- every one is a child widget and Qt
// deletes them with the window -- so holding them individually bought nothing
// and cost a name in the largest class in the program for each. Worse, the code
// that writes into them had to live wherever they were declared, which is why
// updateChrome was a window method reaching into fourteen widgets.
//
// Grouped here, they can be passed to the code that fills them, and the filling
// becomes a function of what the window is showing rather than of the window.
#pragma once

#include <QAction>
#include <QLabel>
#include <QMap>
#include <QMenuBar>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <vector>

namespace zaro::app::chrome {

/// Every widget the chrome is made of.
struct Bars {
    QMenuBar* menuBar{nullptr};

    /// The title bar: which project, and whether it is saved.
    QLabel* projectLabel{nullptr};
    QLabel* autosaveLabel{nullptr};

    /// The tool bar: the format on the left, the workspace tabs in the middle.
    QLabel* formatLabel{nullptr};
    QMap<QString, QPushButton*> workspaceTabs;
    QMap<QString, QAction*> workspaceActions;
    QStackedWidget* actionStack{nullptr};
    QPushButton* renderButton{nullptr};

    /// The viewer bar: what is being looked at, and how well.
    QLabel* viewerLabel{nullptr};
    QLabel* qualityLabel{nullptr};
    QPushButton* sourceTab{nullptr};
    QPushButton* programTab{nullptr};
    QPushButton* guidesButton{nullptr};

    /// The transport: the timecode, the play button, the scrubber.
    QLabel* timecode{nullptr};
    QLabel* remaining{nullptr};
    QPushButton* playButton{nullptr};
    QSlider* scrubber{nullptr};

    /// The timeline pane's own strip: the tools, snapping and zoom.
    QLabel* timelineLabel{nullptr};
    QPushButton* snapButton{nullptr};
    QSlider* zoomSlider{nullptr};
    /// How tall the rows are drawn, all of them together.
    QSlider* rowHeightSlider{nullptr};
    std::vector<QPushButton*> toolButtons;

    /// The status line along the bottom.
    QLabel* statusLeft{nullptr};
    QLabel* statusMiddle{nullptr};
    QLabel* statusRight{nullptr};

    /// The containers a workspace shows and hides.
    QWidget* viewerWell{nullptr};
    QWidget* viewerBar{nullptr};
    QLabel* noMonitorLabel{nullptr};
    QStackedWidget* workspaceStack{nullptr};
    QWidget* audioSide{nullptr};
    QWidget* nodesBox{nullptr};
    QWidget* timelinePane{nullptr};
};

/// What the bars say, gathered from where each fact lives.
///
/// Plain values rather than the objects they came from. That is the whole point
/// of the split: filling a status line is string formatting, and string
/// formatting that takes a project, a timeline widget and a render cache cannot
/// be read or checked without all three.
struct Status {
    QString projectName{"Untitled"};
    QString sequenceName;
    bool haveSequence{false};
    bool modified{false};

    std::int32_t width{0};
    std::int32_t height{0};
    double frameRate{0.0};
    QString durationTimecode;

    bool comparing{false};
    QString toolName;
    QString workspace;
    int binItems{0};
    bool snapEnabled{true};
    std::size_t toolIndex{0};
    double zoomFraction{0.0};
    double trackHeightFraction{0.0};

    /// Deliver shows its queue where the others show the bin, and its range
    /// where they show the format.
    bool inDeliver{false};
    QString deliverStatus;
    QString deliverRange;
    bool rendering{false};

    QString platformLabel;

    /// Set once a device has been asked for and refused. The clock is the audio
    /// device (ADR-006), so without one the playhead does not move -- which is
    /// indistinguishable from a hung transport unless it is said out loud.
    bool audioDeviceMissing{false};
};

/// Put everything the bars say back in step with what the window is showing.
void refresh(const Bars& bars, const Status& status);

}  // namespace zaro::app::chrome
