#include "zaro/ui/Keymap.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>

#include "zaro/ui/Actions.h"

namespace zaro::ui {
namespace {

[[nodiscard]] std::string lowered(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char letter : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(letter))));
    }
    return out;
}

[[nodiscard]] std::string trimmed(std::string_view text) {
    std::size_t from = 0;
    std::size_t to = text.size();
    while (from < to && std::isspace(static_cast<unsigned char>(text[from])) != 0) {
        ++from;
    }
    while (to > from && std::isspace(static_cast<unsigned char>(text[to - 1])) != 0) {
        --to;
    }
    return std::string{text.substr(from, to - from)};
}

/// The keys with names, and every spelling of them worth accepting.
///
/// A table rather than a parser: the set of named keys is small, closed, and
/// the same on every platform this runs on, and a table is the version somebody
/// can read to find out what to type.
struct NamedKey {
    std::string_view canonical;
    std::string_view spelling;
};

constexpr std::array kNamedKeys{
    NamedKey{"Space", "space"},   NamedKey{"Space", "spacebar"},
    NamedKey{"Left", "left"},     NamedKey{"Right", "right"},
    NamedKey{"Up", "up"},         NamedKey{"Down", "down"},
    NamedKey{"Home", "home"},     NamedKey{"End", "end"},
    NamedKey{"PageUp", "pageup"}, NamedKey{"PageDown", "pagedown"},
    NamedKey{"Return", "return"}, NamedKey{"Return", "enter"},
    NamedKey{"Escape", "escape"}, NamedKey{"Escape", "esc"},
    NamedKey{"Tab", "tab"},       NamedKey{"Backspace", "backspace"},
    NamedKey{"Delete", "delete"}, NamedKey{"Delete", "del"},
    NamedKey{"Comma", ","},       NamedKey{"Comma", "comma"},
    NamedKey{"Period", "."},      NamedKey{"Period", "period"},
    NamedKey{"Slash", "/"},       NamedKey{"Backslash", "\\"},
    NamedKey{"Semicolon", ";"},   NamedKey{"'", "'"},
    NamedKey{"[", "["},           NamedKey{"]", "]"},
    NamedKey{"-", "-"},           NamedKey{"-", "minus"},
    NamedKey{"=", "="},           NamedKey{"=", "equal"},
    NamedKey{"=", "equals"},      NamedKey{"`", "`"},
};

[[nodiscard]] std::string canonicalKey(std::string_view token) {
    const std::string lower = lowered(token);
    for (const NamedKey& named : kNamedKeys) {
        if (lower == named.spelling) {
            return std::string{named.canonical};
        }
    }
    // Function keys, by number rather than by name, so F1 to F35 need no table.
    if (lower.size() >= 2 && lower.front() == 'f' &&
        std::all_of(lower.begin() + 1, lower.end(), [](char digit) {
            return std::isdigit(static_cast<unsigned char>(digit)) != 0;
        })) {
        return "F" + lower.substr(1);
    }
    if (lower.size() == 1 && std::isalnum(static_cast<unsigned char>(lower.front())) != 0) {
        return std::string{
            static_cast<char>(std::toupper(static_cast<unsigned char>(lower.front())))};
    }
    return {};
}

}  // namespace

Result<std::string> normaliseShortcut(std::string_view text) {
    const std::string whole = trimmed(text);
    if (whole.empty()) {
        return Error{ErrorCode::InvalidData, "that is not a keystroke"};
    }

    bool control = false;
    bool alt = false;
    bool shift = false;
    bool meta = false;
    std::string key;

    std::string token;
    std::istringstream parts{whole};
    // Split on '+', but a lone '+' *is* a key, so an empty token between two
    // separators means somebody pressed the plus key rather than mistyping.
    std::vector<std::string> tokens;
    std::string current;
    for (std::size_t i = 0; i < whole.size(); ++i) {
        if (whole[i] == '+' && !current.empty()) {
            tokens.push_back(current);
            current.clear();
            continue;
        }
        if (whole[i] == '+' && current.empty() && i + 1 == whole.size()) {
            tokens.emplace_back("+");
            continue;
        }
        current.push_back(whole[i]);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }

    for (const std::string& piece : tokens) {
        const std::string lower = lowered(trimmed(piece));
        if (lower == "ctrl" || lower == "control") {
            control = true;
        } else if (lower == "alt" || lower == "option" || lower == "opt") {
            alt = true;
        } else if (lower == "shift") {
            shift = true;
        } else if (lower == "meta" || lower == "cmd" || lower == "command" || lower == "super") {
            meta = true;
        } else if (!key.empty()) {
            return Error{ErrorCode::InvalidData, "a shortcut can only have one key in it"};
        } else {
            key = canonicalKey(trimmed(piece));
            if (key.empty()) {
                return Error{ErrorCode::InvalidData, "\"" + trimmed(piece) + "\" is not a key"};
            }
        }
    }
    if (key.empty()) {
        // Modifiers alone are a state, not a keystroke: nothing can be bound to
        // "hold shift", and accepting it would produce a shortcut that fires
        // the moment somebody reaches for another one.
        return Error{ErrorCode::InvalidData, "that is only modifiers -- it needs a key too"};
    }

    std::string out;
    if (control) {
        out += "Ctrl+";
    }
    if (alt) {
        out += "Alt+";
    }
    if (shift) {
        out += "Shift+";
    }
    if (meta) {
        out += "Meta+";
    }
    out += key;
    return out;
}

Keymap::Keymap() = default;

std::string Keymap::shortcutFor(std::string_view id) const {
    if (const auto found = changed_.find(id); found != changed_.end()) {
        return found->second;
    }
    if (const ActionInfo* action = findAction(id)) {
        return std::string{action->defaultShortcut};
    }
    return {};
}

bool Keymap::isDefault(std::string_view id) const {
    return changed_.find(id) == changed_.end();
}

std::string Keymap::actionFor(std::string_view shortcut) const {
    if (shortcut.empty()) {
        return {};
    }
    for (const ActionInfo& action : allActions()) {
        if (shortcutFor(action.id) == shortcut) {
            return std::string{action.id};
        }
    }
    return {};
}

Status Keymap::setShortcut(std::string_view id, std::string_view shortcut) {
    if (findAction(id) == nullptr) {
        return Error{ErrorCode::NotFound, "there is no such action"};
    }
    auto normalised = normaliseShortcut(shortcut);
    if (!normalised) {
        return normalised.error();
    }
    if (const std::string holder = actionFor(*normalised); !holder.empty() && holder != id) {
        const ActionInfo* other = findAction(holder);
        return Error{
            ErrorCode::InvalidData,
            *normalised + " is already " + std::string{other != nullptr ? other->label : holder}};
    }
    changed_[std::string{id}] = *normalised;
    return {};
}

Status Keymap::clearShortcut(std::string_view id) {
    if (findAction(id) == nullptr) {
        return Error{ErrorCode::NotFound, "there is no such action"};
    }
    changed_[std::string{id}] = {};
    return {};
}

Status Keymap::resetToDefault(std::string_view id) {
    if (findAction(id) == nullptr) {
        return Error{ErrorCode::NotFound, "there is no such action"};
    }
    changed_.erase(std::string{id});
    return {};
}

void Keymap::resetAll() {
    changed_.clear();
}

std::string Keymap::encode() const {
    std::ostringstream out;
    out << "# CutReel keymap. One line per changed shortcut: action-id = keystroke.\n";
    out << "# An empty keystroke means the action has none. Delete a line to go\n";
    out << "# back to the default.\n";
    for (const auto& [id, shortcut] : changed_) {
        out << id << " = " << shortcut << "\n";
    }
    for (const auto& [id, shortcut] : foreign_) {
        out << id << " = " << shortcut << "\n";
    }
    return out.str();
}

Result<Keymap> Keymap::decode(std::string_view text) {
    Keymap keymap;
    std::istringstream lines{std::string{text}};
    std::string line;
    while (std::getline(lines, line)) {
        const std::string clean = trimmed(line);
        if (clean.empty() || clean.front() == '#') {
            continue;
        }
        const std::size_t equals = clean.find('=');
        if (equals == std::string::npos) {
            return Error{ErrorCode::InvalidData, "this line is not id = keystroke: " + clean};
        }
        const std::string id = trimmed(std::string_view{clean}.substr(0, equals));
        const std::string shortcut = trimmed(std::string_view{clean}.substr(equals + 1));
        if (id.empty()) {
            return Error{ErrorCode::InvalidData, "a line has no action on it: " + clean};
        }
        if (findAction(id) == nullptr) {
            // From a later version. Kept rather than dropped: somebody who
            // opens an older build should not lose bindings for commands it has
            // never heard of.
            keymap.foreign_[id] = shortcut;
            continue;
        }
        if (shortcut.empty()) {
            keymap.changed_[id] = {};
            continue;
        }
        auto normalised = normaliseShortcut(shortcut);
        if (!normalised) {
            return normalised.error();
        }
        keymap.changed_[id] = *normalised;
    }
    return keymap;
}

}  // namespace zaro::ui
