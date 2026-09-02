// Small widget factories, and the bars that are only widgets.
//
// The factories were window methods for one reason: `new QPushButton(this)`.
// They take the parent as an argument instead, which is all "this" ever was to
// them, and a bar that is a row of labels can then be built anywhere.

#include "Widgets.h"

#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QSize>
#include <QSizePolicy>
#include <QSlider>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <string>

#include "../Icons.h"
#include "../SupportButton.h"
#include "../Theme.h"

namespace zaro::app::chrome {

QPushButton* button(QWidget* parent, const QString& text, const QString& tip, bool checkable) {
    auto* made = new QPushButton(text, parent);
    made->setToolTip(tip);
    made->setProperty("flat", true);
    made->setCheckable(checkable);
    made->setFocusPolicy(Qt::NoFocus);
    made->setMinimumWidth(0);
    return made;
}

QPushButton* iconButton(QWidget* parent, app::icons::Glyph glyph, const QString& tip,
                        bool checkable) {
    QPushButton* made = button(parent, {}, tip, checkable);
    made->setIcon(app::icons::toolIcon(glyph));
    made->setIconSize(QSize(17, 17));
    made->setFixedSize(29, 26);
    return made;
}

QFrame* separator(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setFrameShape(QFrame::VLine);
    line->setFixedWidth(1);
    line->setStyleSheet(
        QString("background:%1;border:none").arg(app::theme::divider().name(QColor::HexRgb)));
    line->setFixedHeight(20);
    return line;
}

QLabel* mutedLabel(QWidget* parent, const QString& text) {
    auto* label = new QLabel(text, parent);
    label->setProperty("muted", true);
    return label;
}

QWidget* buildTitleBar(QWidget* parent, Bars& bars) {
    auto* bar = new QWidget(parent);
    bar->setObjectName("chrome-titlebar");
    bar->setFixedHeight(38);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(12, 0, 12, 0);
    row->setSpacing(8);

    auto* brand = new QLabel("CutReel", bar);
    brand->setObjectName("chrome-brand");
    row->addWidget(brand);
    if (!bars.menuBar->isNativeMenuBar()) {
        bars.menuBar->setParent(bar);
        row->addWidget(bars.menuBar);
    }
    row->addStretch(1);

    bars.projectLabel = mutedLabel(bar);
    row->addWidget(bars.projectLabel);
    row->addStretch(1);

    bars.autosaveLabel = mutedLabel(bar);
    row->addWidget(bars.autosaveLabel);
    return bar;
}

QWidget* buildStatusBar(QWidget* parent, Bars& bars) {
    auto* bar = new QWidget(parent);
    bar->setObjectName("chrome-statusbar");
    bar->setFixedHeight(26);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(12, 0, 12, 0);
    row->setSpacing(16);
    bars.statusLeft = mutedLabel(bar);
    bars.statusMiddle = mutedLabel(bar);
    bars.statusRight = mutedLabel(bar);
    row->addWidget(bars.statusLeft);
    row->addWidget(bars.statusMiddle);
    row->addStretch(1);
    row->addWidget(bars.statusRight);
    return bar;
}

QWidget* buildToolPalette(QWidget* parent, Bars& bars,
                          const std::function<void(app::TimelineWidget::Tool)>& chooseTool) {
    // The tools, in the order a cut is made: pick, cut, trim, slip, then the
    // two that move the view rather than the cut.
    struct ToolEntry {
        app::TimelineWidget::Tool tool;
        app::icons::Glyph glyph;
        const char* name;
        const char* key;
    };
    static const ToolEntry kTools[] = {
        {app::TimelineWidget::Tool::Select, app::icons::Glyph::Cursor, "Select", "V"},
        {app::TimelineWidget::Tool::Blade, app::icons::Glyph::Scissors, "Blade", "B"},
        {app::TimelineWidget::Tool::Trim, app::icons::Glyph::TrimEdges, "Trim", "T"},
        {app::TimelineWidget::Tool::Slip, app::icons::Glyph::SlipArrows, "Slip", "Y"},
        {app::TimelineWidget::Tool::Hand, app::icons::Glyph::Hand, "Hand", "H"},
        {app::TimelineWidget::Tool::Zoom, app::icons::Glyph::Magnifier, "Zoom", "Z"},
    };
    // No box around it. In the tool bar it needed one to say where the group
    // ended; on the timeline header it is among the other buttons that act on
    // the timeline, and a border there would be drawing a line between things
    // that belong together.
    auto* toolGroup = new QWidget(parent);
    auto* toolRow = new QHBoxLayout(toolGroup);
    toolRow->setContentsMargins(2, 2, 2, 2);
    toolRow->setSpacing(2);
    for (const ToolEntry& entry : kTools) {
        // The tooltip carries the key. A tool palette is aimed at rather than
        // read -- the shape is what somebody learns -- and the letter is one
        // hover away for as long as it takes to learn it.
        QPushButton* made = iconButton(toolGroup, entry.glyph,
                                       QString("%1 tool (%2)").arg(entry.name, entry.key), true);
        const auto tool = entry.tool;
        QObject::connect(made, &QPushButton::clicked, toolGroup,
                         [chooseTool, tool] { chooseTool(tool); });
        bars.toolButtons.push_back(made);
        toolRow->addWidget(made);
    }
    return toolGroup;
}

namespace {

/// Wire a button to a command, so a bar never says what a command does.
void runs(QPushButton* button, ActionRouter& router, const char* actionId) {
    QObject::connect(button, &QPushButton::clicked, button,
                     [&router, id = std::string{actionId}] { router.trigger(id); });
}

}  // namespace

QWidget* buildToolBar(QWidget* parent, Bars& bars, ActionRouter& router, const Hooks& hooks,
                      const QStringList& workspaces, const QString& supportUrl) {
    auto* bar = new QWidget(parent);
    bar->setObjectName("chrome-toolbar");
    bar->setFixedHeight(46);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(12, 0, 12, 0);
    row->setSpacing(10);

    // Snapping and markers used to be here. They belong with the timeline --
    // both of them are about where an edit lands, and the timeline is where
    // edits land -- so they moved down with the tools.
    bars.formatLabel = mutedLabel(bar);
    row->addWidget(bars.formatLabel);
    row->addStretch(1);

    auto* tabGroup = new QWidget(bar);
    tabGroup->setObjectName("tab-group");
    // Fixed, and centred: without a height the group stretches to the whole
    // bar, so its pill ran from the top edge to the bottom while the buttons
    // beside it were thirty pixels tall in the middle.
    tabGroup->setFixedHeight(30);
    auto* tabRow = new QHBoxLayout(tabGroup);
    tabRow->setContentsMargins(2, 2, 2, 2);
    tabRow->setSpacing(2);
    for (const QString& name : workspaces) {
        QPushButton* tab = button(tabGroup, name, QString("%1 workspace").arg(name), true);
        tab->setFixedHeight(26);
        const auto choose = hooks.chooseWorkspace;
        QObject::connect(tab, &QPushButton::clicked, tab, [choose, name] { choose(name); });
        bars.workspaceTabs.insert(name, tab);
        tabRow->addWidget(tab);
    }
    row->addWidget(tabGroup, 0, Qt::AlignVCenter);
    row->addStretch(1);

    // Two clusters, one shown at a time: Import and Export belong to the
    // workspaces where there is something to import into, and the queue buttons
    // belong to Deliver. A bar that showed all four would offer Export and
    // Start render side by side, which are the same intention asked twice.
    bars.actionStack = new QStackedWidget(bar);
    bars.actionStack->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    auto* editActions = new QWidget(bars.actionStack);
    auto* editRow = new QHBoxLayout(editActions);
    editRow->setContentsMargins(0, 0, 0, 0);
    editRow->setSpacing(8);
    auto* importButton = new QPushButton("Import", editActions);
    importButton->setFixedHeight(30);
    runs(importButton, router, "import-media");
    editRow->addWidget(importButton);
    auto* exportButton = new QPushButton("Export", editActions);
    exportButton->setProperty("accent", true);
    exportButton->setFixedHeight(30);
    runs(exportButton, router, "export-sequence");
    editRow->addWidget(exportButton);
    bars.actionStack->addWidget(editActions);

    auto* deliverActions = new QWidget(bars.actionStack);
    auto* deliverRow = new QHBoxLayout(deliverActions);
    deliverRow->setContentsMargins(0, 0, 0, 0);
    deliverRow->setSpacing(8);
    auto* addToQueue = new QPushButton("Add to queue", deliverActions);
    addToQueue->setFixedHeight(30);
    QObject::connect(addToQueue, &QPushButton::clicked, addToQueue, hooks.queueRender);
    deliverRow->addWidget(addToQueue);
    bars.renderButton = new QPushButton("Start render", deliverActions);
    bars.renderButton->setProperty("accent", true);
    bars.renderButton->setFixedHeight(30);
    QObject::connect(bars.renderButton, &QPushButton::clicked, bars.renderButton,
                     hooks.toggleRendering);
    deliverRow->addWidget(bars.renderButton);
    bars.actionStack->addWidget(deliverActions);
    row->addWidget(bars.actionStack);

    auto* donate = new app::SupportButton(bar);
    donate->setObjectName("donate");
    donate->setText("Donate");
    donate->setToolTip(QString("Support CutReel — opens %1").arg(supportUrl));
    donate->setFixedHeight(30);
    QObject::connect(donate, &QPushButton::clicked, donate, [supportUrl] {
        static_cast<void>(QDesktopServices::openUrl(QUrl{supportUrl}));
    });
    row->addWidget(donate);
    return bar;
}

QWidget* buildViewerBar(QWidget* parent, Bars& bars, ActionRouter& router, const Hooks& hooks) {
    auto* bar = new QWidget(parent);
    bar->setObjectName("chrome-viewer-bar");
    bar->setFixedHeight(34);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(12, 0, 12, 0);
    row->setSpacing(8);

    auto* segment = new QWidget(bar);
    segment->setObjectName("segment-group");
    segment->setFixedHeight(28);
    auto* segmentRow = new QHBoxLayout(segment);
    segmentRow->setContentsMargins(2, 2, 2, 2);
    segmentRow->setSpacing(2);
    bars.sourceTab = button(segment, "Source", "The clip opened from the bin", true);
    bars.programTab = button(segment, "Program", "The sequence at the playhead", true);
    // The dot is what says "toggle" rather than "tab": two tabs in a group mean
    // one of them is on, and these two are independent.
    for (QPushButton* tab : {bars.sourceTab, bars.programTab}) {
        tab->setFixedHeight(24);
        tab->setIconSize(QSize(13, 13));
        segmentRow->addWidget(tab);
    }
    QObject::connect(bars.sourceTab, &QPushButton::toggled, bars.sourceTab, hooks.showSource);
    QObject::connect(bars.programTab, &QPushButton::toggled, bars.programTab, hooks.showProgram);
    row->addWidget(segment, 0, Qt::AlignVCenter);

    bars.viewerLabel = mutedLabel(bar);
    row->addWidget(bars.viewerLabel);
    row->addStretch(1);

    bars.guidesButton = button(bar, "Guides", "Action-safe, title-safe and the thirds", true);
    bars.guidesButton->setFixedHeight(24);
    const auto setGuides = hooks.setGuides;
    QObject::connect(bars.guidesButton, &QPushButton::clicked, bars.guidesButton,
                     [&router, setGuides](bool on) {
                         setGuides(on);
                         // The same command has a menu item with a tick on it.
                         if (QAction* guides = router.find("safe-guides")) {
                             guides->setChecked(on);
                         }
                     });
    row->addWidget(bars.guidesButton);

    bars.qualityLabel = mutedLabel(bar);
    row->addWidget(bars.qualityLabel);
    return bar;
}

QWidget* buildTransportBar(QWidget* parent, Bars& bars, ActionRouter& router) {
    auto* bar = new QWidget(parent);
    bar->setObjectName("chrome-transport");
    auto* column = new QVBoxLayout(bar);
    column->setContentsMargins(14, 6, 14, 8);
    column->setSpacing(6);
    column->addWidget(bars.scrubber);

    auto* row = new QHBoxLayout;
    row->setSpacing(4);
    bars.timecode->setMinimumWidth(140);
    row->addWidget(bars.timecode);
    row->addStretch(1);

    // Every one of these is a command with a keystroke of its own, so the
    // button names the id and nothing else. It used to hold a pointer to a
    // member function, which is a second way of saying "go to the start" that
    // had to be kept in step with the first.
    struct TransportEntry {
        const char* glyph;
        const char* tip;
        const char* actionId;
    };
    static const TransportEntry kBefore[] = {
        {"|◀", "Go to the start (Home)", "go-to-start"},
        {"◁", "Back one frame (Left)", "step-back"},
    };
    static const TransportEntry kAfter[] = {
        {"▷", "Forward one frame (Right)", "step-forward"},
        {"▶|", "Go to the end (End)", "go-to-end"},
    };
    const auto addTransport = [&](const TransportEntry& entry) {
        QPushButton* made = button(bar, entry.glyph, entry.tip);
        made->setFixedSize(32, 30);
        runs(made, router, entry.actionId);
        row->addWidget(made);
    };
    for (const TransportEntry& entry : kBefore) {
        addTransport(entry);
    }
    row->addWidget(bars.playButton);
    for (const TransportEntry& entry : kAfter) {
        addTransport(entry);
    }

    row->addWidget(separator(bar));
    QPushButton* markIn = button(bar, "[", "Mark in (I)");
    markIn->setFixedSize(30, 30);
    runs(markIn, router, "mark-in");
    row->addWidget(markIn);
    QPushButton* markOut = button(bar, "]", "Mark out (O)");
    markOut->setFixedSize(30, 30);
    runs(markOut, router, "mark-out");
    row->addWidget(markOut);

    row->addStretch(1);
    bars.remaining->setMinimumWidth(140);
    bars.remaining->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(bars.remaining);
    column->addLayout(row);
    return bar;
}

QWidget* buildTimelinePane(QWidget* parent, Bars& bars, ActionRouter& router, const Hooks& hooks,
                           QWidget* timelineWidget) {
    auto* pane = new QWidget(parent);
    auto* column = new QVBoxLayout(pane);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    auto* bar = new QWidget(pane);
    bar->setObjectName("chrome-timeline-bar");
    bar->setFixedHeight(34);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(10, 0, 10, 0);
    row->setSpacing(8);
    auto* title = new QLabel("Timeline", bar);
    row->addWidget(title);
    bars.timelineLabel = mutedLabel(bar);
    row->addWidget(bars.timelineLabel);
    row->addWidget(separator(bar));
    row->addWidget(buildToolPalette(bar, bars, hooks.chooseTool));

    QPushButton* razor = iconButton(bar, app::icons::Glyph::Split, "Razor at the playhead (C)");
    runs(razor, router, "razor");
    row->addWidget(razor);

    QPushButton* dissolve =
        iconButton(bar, app::icons::Glyph::CrossFade, "Put a dissolve on the cut at the playhead");
    runs(dissolve, router, "add-dissolve");
    row->addWidget(dissolve);

    row->addWidget(separator(bar));

    bars.snapButton = iconButton(bar, app::icons::Glyph::Magnet,
                                 "Pull edits to the edit points near them (S)", true);
    QObject::connect(bars.snapButton, &QPushButton::clicked, bars.snapButton, hooks.setSnapEnabled);
    row->addWidget(bars.snapButton);

    QPushButton* markerButton =
        iconButton(bar, app::icons::Glyph::Bookmark, "Add a marker at the playhead (M)");
    runs(markerButton, router, "add-marker");
    row->addWidget(markerButton);

    row->addStretch(1);
    bars.snapLabel = mutedLabel(bar);
    row->addWidget(bars.snapLabel);

    // How tall the rows are, beside how wide the time is: the two questions a
    // timeline is read at, and the same shape of control for both. Rows keep
    // whatever heights they were given one by one -- this scales them together,
    // so a row made tall to work on stays the tallest of them.
    auto* rowsLabel = mutedLabel(bar);
    rowsLabel->setText("Rows");
    row->addWidget(rowsLabel);
    bars.rowHeightSlider = new QSlider(Qt::Horizontal, bar);
    bars.rowHeightSlider->setObjectName("row-height-slider");
    bars.rowHeightSlider->setFixedWidth(72);
    bars.rowHeightSlider->setRange(0, 1000);
    bars.rowHeightSlider->setFocusPolicy(Qt::NoFocus);
    bars.rowHeightSlider->setToolTip("How tall the tracks are drawn");
    const auto setRowHeight = hooks.setTrackHeightFraction;
    // Every way of moving it counts, not only a drag of the handle: a click on
    // the groove pages it, and the arrow keys step it. There is no feedback to
    // guard against -- the status pass that follows the model blocks the
    // slider's signals while it writes to it.
    QObject::connect(bars.rowHeightSlider, &QSlider::valueChanged, bars.rowHeightSlider,
                     [setRowHeight](int value) {
                         if (setRowHeight) {
                             setRowHeight(value / 1000.0);
                         }
                     });
    row->addWidget(bars.rowHeightSlider);
    row->addWidget(separator(bar));
    auto* zoomOut = iconButton(bar, app::icons::Glyph::Minus, "Zoom out (−)");
    runs(zoomOut, router, "zoom-out");
    row->addWidget(zoomOut);
    bars.zoomSlider = new QSlider(Qt::Horizontal, bar);
    bars.zoomSlider->setFixedWidth(96);
    bars.zoomSlider->setRange(0, 1000);
    bars.zoomSlider->setFocusPolicy(Qt::NoFocus);
    const auto setZoom = hooks.setZoomFraction;
    QObject::connect(bars.zoomSlider, &QSlider::valueChanged, bars.zoomSlider,
                     [&bars, setZoom](int value) {
                         if (bars.zoomSlider->isSliderDown()) {
                             setZoom(value / 1000.0);
                         }
                     });
    row->addWidget(bars.zoomSlider);
    auto* zoomIn = iconButton(bar, app::icons::Glyph::Plus, "Zoom in (+)");
    runs(zoomIn, router, "zoom-in");
    row->addWidget(zoomIn);

    column->addWidget(bar);
    column->addWidget(timelineWidget, 1);
    return pane;
}

}  // namespace zaro::app::chrome
