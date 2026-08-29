#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "zaro/core/Error.h"

namespace zaro::ui {

/// Put a shortcut into the one form everything else compares against.
///
/// **Because a conflict check is a string comparison.** "ctrl+S", "Control+s"
/// and "Ctrl+S" are one shortcut, and a keymap that stored them as three could
/// bind three actions to the same keystroke while reporting no clash. One
/// spelling, decided here: modifiers in the order Ctrl, Alt, Shift, Meta, then
/// the key.
///
/// Accepts what people and platforms actually write -- `cmd` and `command` for
/// Meta, `option` for Alt, `esc` for Escape -- and refuses what is not a
/// keystroke at all: an empty string, modifiers with no key, a key nobody has.
[[nodiscard]] Result<std::string> normaliseShortcut(std::string_view text);

/// Which keystroke runs which action.
///
/// Starts as the catalogue's defaults and remembers only what somebody changed,
/// which is what makes a keymap file readable, small, and survivable when the
/// defaults move: a shortcut nobody customised follows the application, and one
/// they did stays theirs.
class Keymap {
public:
    Keymap();

    /// Empty when the action has no shortcut, which is a real state and not an
    /// error: most menu items live without one.
    [[nodiscard]] std::string shortcutFor(std::string_view id) const;
    [[nodiscard]] bool isDefault(std::string_view id) const;

    /// Bind a keystroke, refusing one that is already in use.
    ///
    /// **Refused rather than stolen.** Silently unbinding whatever held it is
    /// how somebody loses a shortcut they use daily and cannot work out where
    /// it went; the error names the action holding it, so the fix is one step
    /// and their decision. `clearShortcut` is the way to free one.
    [[nodiscard]] Status setShortcut(std::string_view id, std::string_view shortcut);
    [[nodiscard]] Status clearShortcut(std::string_view id);
    [[nodiscard]] Status resetToDefault(std::string_view id);
    void resetAll();

    /// The action bound to a keystroke, or empty. This is what a key press is
    /// answered with.
    [[nodiscard]] std::string actionFor(std::string_view shortcut) const;

    /// What is written to disk: the differences, and anything a later version
    /// left here that this build does not recognise.
    ///
    /// **Unknown ids are kept.** Somebody who opens their project in an older
    /// build and changes one shortcut should not lose the bindings for commands
    /// that build has never heard of -- the same reason project files carry
    /// their unknown fields through a save.
    [[nodiscard]] std::string encode() const;
    [[nodiscard]] static Result<Keymap> decode(std::string_view text);

private:
    /// Only what differs from the catalogue.
    std::map<std::string, std::string, std::less<>> changed_;
    std::map<std::string, std::string, std::less<>> foreign_;
};

}  // namespace zaro::ui
