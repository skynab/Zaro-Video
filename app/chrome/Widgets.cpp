// Small widget factories, and the bars that are only widgets.
//
// The factories were window methods for one reason: `new QPushButton(this)`.
// They take the parent as an argument instead, which is all "this" ever was to
// them, and a bar that is a row of labels can then be built anywhere.

#include "Widgets.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QComboBox>
#include <QDesktopServices>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QPainter>
#include <QRect>
#include <QSize>
#include <QSizePolicy>
#include <QSlider>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <functional>
#include <string>

#include "../Icons.h"
#include "../SupportButton.h"
#include "../Theme.h"
#include "Choices.h"

namespace zaro::app::chrome {
namespace {

/// Runs something whenever the widget it watches is resized.
///
/// A filter rather than a resizeEvent override, because the point of the
/// helpers that use it -- `setEmptyText`, `setElidedText` -- is that they work
/// on the widgets the panels already build. A subclass would mean changing
/// every one of them to use it.
class ResizeWatcher : public QObject {
public:
    ResizeWatcher(QWidget* subject, std::function<void()> sync, QObject* parent)
        : QObject{parent}, subject_{subject}, sync_{std::move(sync)} {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Resize && subject_ != nullptr) {
            sync_();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QWidget* subject_{nullptr};
    std::function<void()> sync_;
};

}  // namespace

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

void setElidedText(QLabel* label, const QString& text, Qt::TextElideMode mode) {
    if (label == nullptr) {
        return;
    }
    label->setToolTip(text);
    // Ignored horizontally: the point is that the label stops asking for the
    // width of its whole string, which is what was widening the row.
    label->setSizePolicy(QSizePolicy::Ignored, label->sizePolicy().verticalPolicy());
    const auto paint = [label, text, mode] {
        label->setText(label->fontMetrics().elidedText(text, mode, std::max(0, label->width())));
    };
    label->installEventFilter(new ResizeWatcher{label, paint, label});
    paint();
}

QString humanSize(double bytes) {
    constexpr double kUnit = 1024.0;
    if (bytes >= kUnit * kUnit * kUnit) {
        return QString("%1 GB").arg(bytes / (kUnit * kUnit * kUnit), 0, 'f', 1);
    }
    if (bytes >= kUnit * kUnit) {
        return QString("%1 MB").arg(bytes / (kUnit * kUnit), 0, 'f', 1);
    }
    // Down to kilobytes, and no further: a file smaller than a kilobyte is not
    // one anybody is deciding anything about, and "0 kB" says enough.
    return QString("%1 kB").arg(bytes / kUnit, 0, 'f', 0);
}

void setEmptyText(QAbstractItemView* view, const QString& text) {
    if (view == nullptr || view->viewport() == nullptr) {
        return;
    }
    auto* label = new QLabel(text, view->viewport());
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    label->setContentsMargins(16, 0, 16, 0);
    label->setProperty("muted", true);
    // The list is still the thing being clicked and scrolled; this is only
    // something to read while there is nothing to click.
    label->setAttribute(Qt::WA_TransparentForMouseEvents);

    const auto sync = [view, label] {
        const QAbstractItemModel* model = view->model();
        const bool empty = model == nullptr || model->rowCount(view->rootIndex()) == 0;
        label->setGeometry(view->viewport()->rect());
        label->setVisible(empty);
        if (empty) {
            label->raise();
        }
    };

    // Follow the viewport's size, and the model's contents. Both matter: a
    // label left at its first geometry sits in the corner of a resized panel,
    // and one that never hears about a row stays up over the list it is
    // covering.
    QObject::connect(view->model(), &QAbstractItemModel::rowsInserted, label, sync);
    QObject::connect(view->model(), &QAbstractItemModel::rowsRemoved, label, sync);
    QObject::connect(view->model(), &QAbstractItemModel::modelReset, label, sync);
    view->viewport()->installEventFilter(new ResizeWatcher{label, sync, view->viewport()});
    sync();
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
    // Named: it is the readout somebody checks before deciding whether to
    // press Save, so it is worth being able to assert on it.
    bars.autosaveLabel->setObjectName("autosave-label");
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

/// The Add Title tooltip, with whatever key is actually bound to it.
///
/// Written from the keymap rather than spelled out, for two reasons: the
/// shortcut is rebindable, and `Ctrl+Shift+T` is not what a Mac shows -- Qt
/// renders the same binding as the platform writes it.
QString titleTip(const std::string& shortcut) {
    const QString base = QStringLiteral("Add a title at the playhead");
    if (shortcut.empty()) {
        return base;
    }
    const QString key =
        QKeySequence::fromString(QString::fromStdString(shortcut), QKeySequence::PortableText)
            .toString(QKeySequence::NativeText);
    return key.isEmpty() ? base : QStringLiteral("%1 (%2)").arg(base, key);
}

/// A readout you can press: it says a fact and opens the thing that sets it.
/// Styled as the muted text beside it rather than as a button, so the bar still
/// reads as a bar -- the pointer and the hover are what say it can be pressed.
QPushButton* readout(QWidget* parent, const QString& tip, ActionRouter& router,
                     const char* actionId) {
    auto* made = new QPushButton(parent);
    made->setObjectName("chrome-readout");
    made->setFlat(true);
    made->setProperty("flat", true);
    made->setCursor(Qt::PointingHandCursor);
    made->setFocusPolicy(Qt::NoFocus);
    made->setToolTip(tip);
    runs(made, router, actionId);
    return made;
}

/// The frame-size dropdown, with the chevron the rest of this bar uses.
///
/// Nothing in this program draws a combo box arrow: the stylesheet gives every
/// QComboBox a bare drop-down area and no image for the indicator, so a combo
/// in the toolbar is a rounded rectangle with a number in it and nothing says
/// it opens. The readouts beside it solve that with a literal "⌄" in their
/// text, and this draws the same glyph in the same place -- matching the bar's
/// existing signal for "this can be opened" rather than inventing a second one.
///
/// Drawn rather than styled because the stylesheet route needs an image asset:
/// the usual CSS zero-size-border triangle is not something Qt's stylesheet
/// engine implements, and asking for one paints a filled square.
class FormatCombo final : public QComboBox {
public:
    using QComboBox::QComboBox;

protected:
    void paintEvent(QPaintEvent* event) override {
        QComboBox::paintEvent(event);
        if (!isEnabled()) {
            return;
        }
        QPainter painter{this};
        painter.setPen(underMouse() ? app::theme::text() : app::theme::textAt(0.55));
        painter.drawText(QRect(width() - 15, 0, 12, height()), Qt::AlignCenter,
                         QStringLiteral("⌄"));
    }
};

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
    // A dropdown rather than another readout button. It is the project's
    // resolution, it is a setting with a value, and the top left of the window
    // is where somebody looks for it -- so it says what it is set to without
    // being clicked, and opens its own list when it is.
    bars.formatBox = new FormatCombo(bar);
    bars.formatBox->setObjectName("chrome-format");
    bars.formatBox->setToolTip(
        "Frame size — the resolution this sequence renders and\n"
        "exports at. Pick one, 1080\u00d71920 vertical included, or\n"
        "Custom\u2026 to type a size. Also at Sequence \u25b8 Frame Size.");
    bars.formatBox->setFocusPolicy(Qt::NoFocus);
    bars.formatBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    {
        // `activated` and not `currentIndexChanged`: the box is repopulated
        // whenever the sequence changes, and a change signal would fire on
        // every one of those and resize the sequence to whatever the refresh
        // had just selected.
        const auto choose = hooks.chooseFrameSize;
        QObject::connect(bars.formatBox, &QComboBox::activated, bars.formatBox,
                         [box = bars.formatBox, choose](int index) {
                             if (!choose) {
                                 return;
                             }
                             const QVariant size = box->itemData(index);
                             if (!size.isValid()) {
                                 return;
                             }
                             const QPoint pair = size.toPoint();
                             choose(pair.x(), pair.y());
                         });
    }

    // The same slot in Deliver, where what belongs there is the render range.
    bars.formatButton = readout(bar,
                                "Frame size — the resolution this sequence renders and\n"
                                "exports at. Click to pick one, 1080\u00d71920 vertical\n"
                                "included. Also at Sequence \u25b8 Frame Size.",
                                router, "frame-size");
    bars.formatButton->hide();
    bars.rateButton = readout(bar,
                              "Frame rate — how fast this sequence plays.\n"
                              "Click to change. Also at Sequence \u25b8 Frame Rate.",
                              router, "frame-rate");
    row->addWidget(bars.formatBox);
    row->addWidget(bars.formatButton);
    row->addWidget(bars.rateButton);
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

    QPushButton* dissolve = iconButton(
        bar, app::icons::Glyph::CrossFade,
        "Put a dissolve on the cut at the playhead, or a fade on a clip's end where there is "
        "no cut. Drag its edge to set how long it lasts.");
    runs(dissolve, router, "add-dissolve");
    row->addWidget(dissolve);

    // Titles were reachable from the bin, the right-click menu and a
    // keystroke, and from nothing anybody looking at the timeline could see --
    // so the one control people went hunting for was the one that was not
    // there. It sits with the razor and the dissolve because all three do the
    // same kind of thing: put something down where the playhead is.
    QPushButton* titleButton = iconButton(bar, app::icons::Glyph::TextT,
                                          titleTip(router.keymap().shortcutFor("add-title")));
    // Named, so the test that presses it can find it. The bin's own Add Title
    // button already answers to "add-title"; this one says where it is, or a
    // search from the window would turn up two buttons and pick either.
    titleButton->setObjectName("timeline-add-title");
    runs(titleButton, router, "add-title");
    row->addWidget(titleButton);

    row->addWidget(separator(bar));

    bars.snapButton = iconButton(bar, app::icons::Glyph::Magnet, "Snap on (S)", true);
    QObject::connect(bars.snapButton, &QPushButton::clicked, bars.snapButton, hooks.setSnapEnabled);
    row->addWidget(bars.snapButton);

    QPushButton* markerButton =
        iconButton(bar, app::icons::Glyph::Bookmark, "Add a marker at the playhead (M)");
    runs(markerButton, router, "add-marker");
    row->addWidget(markerButton);

    row->addStretch(1);

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
    // the groove pages it, and the wheel steps it. A guard on `isSliderDown`
    // would let both of those move the handle and change nothing, and the next
    // status pass would put the handle back -- which reads as a control that
    // ignores you. There is no feedback to guard against either: the pass that
    // follows the model blocks this slider's signals while it writes to it.
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
    // Driven the same way the row-height slider is, for the reason given there:
    // this one used to answer only to a drag of its handle, so a click on the
    // groove moved the handle and left the zoom where it was.
    const auto setZoom = hooks.setZoomFraction;
    QObject::connect(bars.zoomSlider, &QSlider::valueChanged, bars.zoomSlider,
                     [setZoom](int value) {
                         if (setZoom) {
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
