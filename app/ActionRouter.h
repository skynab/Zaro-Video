// Every command the window can run, by the id the menus and the keymap use.
//
// There were two registries for this. A command with a menu item kept its
// handler on a QAction; a command without one -- play, mark in, step a frame --
// kept its handler in a map, because its default keystroke is a bare letter and
// a bare letter cannot be a Qt shortcut without firing while somebody types.
// `trigger` looked in both. Which registry a command lived in was therefore
// decided by whether it happened to appear on a menu, and the two halves drifted
// apart: a command could be rebindable but not triggerable, or listed in the
// keyboard manager and reachable from nothing.
//
// One registry. The chrome names an id and knows nothing else about it; what an
// id *does* is registered once, by the window that knows how to do it. That is
// what lets the menus and the bars be built somewhere other than inside the
// window class.
#pragma once

#include <QAction>
#include <QKeySequence>
#include <QMap>
#include <QObject>
#include <QString>
#include <functional>
#include <map>
#include <string>

#include "zaro/ui/Keymap.h"

namespace zaro::app {

class ActionRouter : public QObject {
    Q_OBJECT

public:
    explicit ActionRouter(QWidget* owner);

    /// Say what an id does.
    ///
    /// Every id must be one `ui::findAction` knows: the catalogue is what the
    /// keyboard manager lists and what a keymap file is checked against, and a
    /// command missing from it is one nobody can find or rebind.
    void bind(const char* actionId, std::function<void()> handler);

    /// Say what an id does, for a command that is on or off.
    ///
    /// The handler is told which it now is. Its QAction is checkable, so a menu
    /// item and a toolbar button for the same id show the same tick.
    void bindToggle(const char* actionId, std::function<void(bool)> handler);

    /// Run a command by id. False when nothing is bound to it.
    ///
    /// A toggle triggered this way flips, which is what a keystroke for one
    /// should do.
    bool trigger(const std::string& actionId);

    /// The QAction for an id, made on first ask.
    ///
    /// One per id, shared by every menu and button that names it -- so a
    /// command that appears in two places is enabled, checked and shortcut in
    /// both without anyone keeping them in step.
    QAction* action(const char* actionId);

    /// The QAction for an id if one has been made, else null.
    [[nodiscard]] QAction* find(const QString& actionId) const;

    [[nodiscard]] ui::Keymap& keymap() noexcept { return keymap_; }
    [[nodiscard]] const ui::Keymap& keymap() const noexcept { return keymap_; }

    /// Put every action's shortcut back in step with the keymap.
    void applyKeymap();

    void loadKeymap();
    void saveKeymap();

    /// Where the keymap is read from and written to.
    ///
    /// Overridable so a test can rebind things without rewriting the keymap of
    /// whoever is running it.
    [[nodiscard]] static QString keymapPath();
    static void setKeymapPath(const QString& path);

private:
    void applyShortcut(const QString& actionId, QAction* action);

    QWidget* owner_{nullptr};
    /// What every command does, by id. A toggle is told its new state; the
    /// rest ignore the argument.
    std::map<std::string, std::function<void(bool)>> handlers_;
    /// Which ids are on-or-off commands.
    std::map<std::string, bool> toggles_;
    /// The QActions made for ids that something asked to show.
    QMap<QString, QAction*> actions_;
    ui::Keymap keymap_;
    static inline QString keymapPath_;
};

}  // namespace zaro::app
