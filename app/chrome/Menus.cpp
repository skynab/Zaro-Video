// The menu bar: which commands appear where.
//
// Structure and nothing else. Every item names a command by id and knows
// nothing about what the command does -- that is registered once, in
// PreviewWindow::bindCommands, and the router hands back an action already
// carrying the right label, shortcut and checked state.
//
// It could live in the window and used to. It does not need to, and a menu bar
// is exactly the kind of thing worth being able to read end to end without
// scrolling past the code that implements every item on it.

#include "Menus.h"

#include <QMenu>

#include "zaro/ui/Actions.h"

namespace zaro::app::chrome {
namespace {

/// Put a command on a menu, by id.
QAction* addItem(ActionRouter& router, QMenu* menu, const char* actionId) {
    QAction* action = router.action(actionId);
    menu->addAction(action);
    return action;
}

}  // namespace

QMenuBar* buildMenuBar(QWidget* parent, ActionRouter& router, const QStringList& workspaces,
                       QMap<QString, QAction*>& workspaceActions,
                       const std::function<void(const QString&)>& chooseWorkspace,
                       const std::function<void(QMenuBar*)>& addWindowMenus) {
    auto* bar = new QMenuBar(parent);
    // Native where the platform has a menu bar of its own. The design draws
    // the menus inside the window, which is right on Windows and Linux and
    // wrong on macOS: there the menu bar belongs at the top of the screen,
    // and a second one in the window is a second place to look. Qt decides
    // by platform on its own; this only says so out loud.
#ifdef Q_OS_MACOS
    bar->setNativeMenuBar(true);
#else
    bar->setNativeMenuBar(false);
#endif

    QMenu* file = bar->addMenu("File");
    addItem(router, file, "new-project");
    addItem(router, file, "open-project");
    // A cut from another program opens the same way a project does, so these
    // sit with Open rather than under Media: what arrives is a timeline, not
    // footage, and the Media submenu is about files to cut with. Two items and
    // not one with a format picker, because the two programs are what somebody
    // is choosing between and the file formats are an implementation detail of
    // that choice.
    addItem(router, file, "import-premiere");
    addItem(router, file, "import-finalcut");
    file->addSeparator();
    addItem(router, file, "save-project");
    addItem(router, file, "save-project-as");

    // Grouped into submenus rather than listed. Fifteen items and four
    // rules made a File menu taller than some of the panels, and length is
    // what makes a menu hard to read: four of these are about media, two
    // about versions, two about templates and two about getting a file
    // out, and saying so is shorter than spelling every one of them out.
    QMenu* versions = file->addMenu("Versions");
    addItem(router, versions, "save-version");
    addItem(router, versions, "open-version");
    file->addSeparator();

    QMenu* media = file->addMenu("Media");
    addItem(router, media, "import-media");
    addItem(router, media, "browse-media");
    media->addSeparator();
    addItem(router, media, "relink-media");
    addItem(router, media, "consolidate-media");

    QMenu* exports = file->addMenu("Export");
    addItem(router, exports, "export-sequence");
    addItem(router, exports, "export-otio");
    addItem(router, exports, "export-premiere");
    addItem(router, exports, "export-finalcut");

    QMenu* templates = file->addMenu("Templates");
    addItem(router, templates, "save-template");
    addItem(router, templates, "place-template");
    file->addSeparator();
    addItem(router, file, "close-window");

    QMenu* edit = bar->addMenu("Edit");
    addItem(router, edit, "undo");
    addItem(router, edit, "redo");
    edit->addSeparator();
    addItem(router, edit, "cut-clips");
    addItem(router, edit, "copy-clips");
    addItem(router, edit, "paste-clips");
    edit->addSeparator();
    addItem(router, edit, "select-all");
    edit->addSeparator();
    addItem(router, edit, "detect-scenes");

    QMenu* clip = bar->addMenu("Clip");
    addItem(router, clip, "match-frame");
    addItem(router, clip, "make-subclip");
    clip->addSeparator();
    addItem(router, clip, "proxies");
    addItem(router, clip, "multicam");
    addItem(router, clip, "captions");
    clip->addSeparator();
    addItem(router, clip, "delete-selected");
    addItem(router, clip, "ripple-delete");

    // The tools have a menu of their own now. They were keys and a row of
    // toolbar buttons, and neither says what the key is -- so the one place
    // that could teach somebody "B is the blade" did not exist.
    QMenu* tools = bar->addMenu("Tools");
    addItem(router, tools, "tool-select");
    addItem(router, tools, "tool-blade");
    addItem(router, tools, "tool-trim");
    addItem(router, tools, "tool-slip");
    addItem(router, tools, "tool-hand");
    tools->addSeparator();
    addItem(router, tools, "toggle-snap");

    QMenu* sequence = bar->addMenu("Sequence");
    addItem(router, sequence, "razor");
    addItem(router, sequence, "add-dissolve");
    sequence->addSeparator();
    // Above Delivery, and next to it, because the three together are "what
    // comes out of this": the size of the picture, how fast it plays, and the
    // curve it is delivered through. Frame size and frame rate used to have no
    // menu item at all, which made the two things that decide the export's
    // format the two things with nowhere to set or even ask about them.
    addItem(router, sequence, "frame-size");
    addItem(router, sequence, "frame-rate");
    addItem(router, sequence, "delivery");
    sequence->addSeparator();
    addItem(router, sequence, "render-range");
    addItem(router, sequence, "loudness");

    QMenu* text = bar->addMenu("Text");
    addItem(router, text, "add-title");
    text->addSeparator();
    // The ready-made moves, on the selected title. Under Text rather than
    // Effects: they are about the way words arrive, not about the picture.
    addItem(router, text, "title-fade-in");
    addItem(router, text, "title-fade-out");
    addItem(router, text, "title-slide-on");
    text->addSeparator();
    addItem(router, text, "show-transcript");

    QMenu* audio = bar->addMenu("Audio");
    addItem(router, audio, "fit-music");

    QMenu* marker = bar->addMenu("Marker");
    addItem(router, marker, "add-marker");
    addItem(router, marker, "next-marker");
    addItem(router, marker, "previous-marker");
    marker->addSeparator();
    addItem(router, marker, "resolve-comment");
    addItem(router, marker, "export-review");

    QMenu* effects = bar->addMenu("Effects");
    addItem(router, effects, "compare");
    addItem(router, effects, "match-shot");

    QMenu* view = bar->addMenu("View");
    QMenu* workspaceMenu = view->addMenu("Workspace");
    for (const QString& name : workspaces) {
        QAction* action = workspaceMenu->addAction(name);
        action->setCheckable(true);
        workspaceActions.insert(name, action);
        QObject::connect(action, &QAction::triggered, bar,
                         [chooseWorkspace, name] { chooseWorkspace(name); });
    }
    view->addSeparator();
    addItem(router, view, "zoom-in");
    addItem(router, view, "zoom-out");
    addItem(router, view, "zoom-fit");
    view->addSeparator();
    addItem(router, view, "safe-guides");

    addWindowMenus(bar);
    return bar;
}

}  // namespace zaro::app::chrome
