#pragma once

#include <span>
#include <string_view>

namespace zaro::ui {

/// One thing the application can be asked to do.
///
/// **Everything a person can trigger is one of these, and it is data.** Before
/// this, a command was a lambda wired to a menu item with a shortcut baked into
/// the call: the list of what the program does existed only as the shape of the
/// code that built the menus, which is why the shortcuts could not be changed
/// and why the Help window's list of them was a hand-written string that could
/// drift from the truth. A catalogue makes the set of commands something that
/// can be read, rebound, searched and tested.
///
/// **The id is the stable name.** Labels get reworded and menus get
/// reorganised; a keymap somebody has customised has to survive both. Ids are
/// kebab-case and never change once shipped -- they are what a keymap file
/// refers to, and renaming one silently unbinds whatever the user had set.
struct ActionInfo {
    std::string_view id;
    std::string_view label;
    /// What it is grouped under, for the manager and for the help list. The
    /// menu it happens to live in is not always the answer: playback has no
    /// menu at all.
    std::string_view category;
    /// What it is bound to out of the box. Empty means nothing, which is a
    /// perfectly good default for the things people reach for through a menu.
    ///
    /// Written in the same normalised form `Keymap` uses, so the catalogue and
    /// a keymap file can be compared without parsing anything twice.
    std::string_view defaultShortcut;
};

/// Every action, once.
///
/// Anything that has to visit them all -- the manager, the help list, the check
/// that every action has something behind it -- uses this, so adding a command
/// is one entry rather than a hunt for the several places that list them.
[[nodiscard]] std::span<const ActionInfo> allActions();

/// The action with this id, or nullptr. Unknown ids come from keymaps written
/// by a later version, and refusing to load one because of a name this build
/// does not know would be worse than ignoring the line.
[[nodiscard]] const ActionInfo* findAction(std::string_view id);

}  // namespace zaro::ui
