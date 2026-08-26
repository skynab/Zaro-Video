#include <algorithm>
#include <set>

#include <catch2/catch_test_macros.hpp>

#include "zaro/ui/Actions.h"
#include "zaro/ui/Keymap.h"

using namespace zaro;

TEST_CASE("every action has a unique id and a label", "[keymap]") {
    std::set<std::string_view> ids;
    for (const ui::ActionInfo& action : ui::allActions()) {
        INFO(action.id);
        CHECK_FALSE(action.id.empty());
        CHECK_FALSE(action.label.empty());
        CHECK_FALSE(action.category.empty());
        CHECK(ids.insert(action.id).second);
    }
    CHECK(ids.size() > 40);
}

TEST_CASE("no two actions ship with the same default shortcut", "[keymap]") {
    std::set<std::string> taken;
    for (const ui::ActionInfo& action : ui::allActions()) {
        if (action.defaultShortcut.empty()) {
            continue;
        }
        INFO(action.id << " -> " << action.defaultShortcut);
        // Two commands on one keystroke is a bug nobody notices until one of
        // them stops working, and it is a bug a list can rule out.
        CHECK(taken.insert(std::string{action.defaultShortcut}).second);
    }
}

TEST_CASE("every default shortcut is already in the form the keymap uses", "[keymap]") {
    for (const ui::ActionInfo& action : ui::allActions()) {
        if (action.defaultShortcut.empty()) {
            continue;
        }
        auto normalised = ui::normaliseShortcut(action.defaultShortcut);
        INFO(action.id << " -> " << action.defaultShortcut);
        REQUIRE(normalised);
        // Otherwise a conflict check would compare a catalogue spelling
        // against a normalised one and miss.
        CHECK(*normalised == action.defaultShortcut);
    }
}

TEST_CASE("one keystroke has one spelling", "[keymap]") {
    CHECK(*ui::normaliseShortcut("ctrl+s") == "Ctrl+S");
    CHECK(*ui::normaliseShortcut("Control+S") == "Ctrl+S");
    CHECK(*ui::normaliseShortcut("  CTRL + s ") == "Ctrl+S");
    // Modifier order is decided here, not by whoever typed it.
    CHECK(*ui::normaliseShortcut("Shift+Ctrl+S") == "Ctrl+Shift+S");
    CHECK(*ui::normaliseShortcut("cmd+shift+alt+k") == "Alt+Shift+Meta+K");
    CHECK(*ui::normaliseShortcut("option+left") == "Alt+Left");
    CHECK(*ui::normaliseShortcut("SPACE") == "Space");
    CHECK(*ui::normaliseShortcut("esc") == "Escape");
    CHECK(*ui::normaliseShortcut("f12") == "F12");
    CHECK(*ui::normaliseShortcut(",") == "Comma");
}

TEST_CASE("what is not a keystroke is refused, by name", "[keymap]") {
    CHECK_FALSE(ui::normaliseShortcut(""));
    CHECK_FALSE(ui::normaliseShortcut("   "));
    // Modifiers alone are a state, not a keystroke.
    CHECK_FALSE(ui::normaliseShortcut("Ctrl"));
    CHECK_FALSE(ui::normaliseShortcut("Ctrl+Shift"));
    CHECK_FALSE(ui::normaliseShortcut("Ctrl+Wobble"));
    CHECK_FALSE(ui::normaliseShortcut("A+B"));
}

TEST_CASE("a fresh keymap is the catalogue", "[keymap]") {
    const ui::Keymap keymap;
    CHECK(keymap.shortcutFor("save-project") == "Ctrl+S");
    CHECK(keymap.shortcutFor("play-pause") == "Space");
    CHECK(keymap.isDefault("save-project"));
    // An action with no default has no shortcut, which is not an error.
    CHECK(keymap.shortcutFor("relink-media").empty());
    // And an id nobody knows has none either.
    CHECK(keymap.shortcutFor("not-an-action").empty());
}

TEST_CASE("a shortcut can be changed, and changed back", "[keymap]") {
    ui::Keymap keymap;
    REQUIRE(keymap.setShortcut("relink-media", "ctrl+shift+r"));
    CHECK(keymap.shortcutFor("relink-media") == "Ctrl+Shift+R");
    CHECK_FALSE(keymap.isDefault("relink-media"));
    CHECK(keymap.actionFor("Ctrl+Shift+R") == "relink-media");

    REQUIRE(keymap.resetToDefault("relink-media"));
    CHECK(keymap.shortcutFor("relink-media").empty());
    CHECK(keymap.isDefault("relink-media"));
}

TEST_CASE("a keystroke already in use is refused, and says by what", "[keymap]") {
    ui::Keymap keymap;
    auto clash = keymap.setShortcut("relink-media", "Ctrl+S");
    REQUIRE_FALSE(clash);
    // Named, so the fix is one step and the user's decision rather than a
    // shortcut they use daily disappearing without explanation.
    CHECK(clash.error().message().find("Save") != std::string::npos);
    CHECK(keymap.shortcutFor("save-project") == "Ctrl+S");

    // Freeing it first is the way through.
    REQUIRE(keymap.clearShortcut("save-project"));
    REQUIRE(keymap.setShortcut("relink-media", "Ctrl+S"));
    CHECK(keymap.actionFor("Ctrl+S") == "relink-media");
}

TEST_CASE("rebinding an action to what it already has is not a conflict", "[keymap]") {
    ui::Keymap keymap;
    CHECK(keymap.setShortcut("save-project", "ctrl+s"));
}

TEST_CASE("only the differences are written, and they come back", "[keymap]") {
    ui::Keymap keymap;
    REQUIRE(keymap.setShortcut("relink-media", "Ctrl+Shift+R"));
    REQUIRE(keymap.clearShortcut("play-pause"));

    const std::string text = keymap.encode();
    // Small, because it holds what somebody changed and nothing else: a
    // shortcut nobody touched follows the application when the defaults move.
    CHECK(text.find("relink-media") != std::string::npos);
    CHECK(text.find("save-project") == std::string::npos);

    auto reloaded = ui::Keymap::decode(text);
    REQUIRE(reloaded);
    CHECK(reloaded->shortcutFor("relink-media") == "Ctrl+Shift+R");
    CHECK(reloaded->shortcutFor("play-pause").empty());
    CHECK(reloaded->shortcutFor("save-project") == "Ctrl+S");
}

TEST_CASE("a binding for an action this build does not know is kept", "[keymap]") {
    auto keymap = ui::Keymap::decode("from-the-future = Ctrl+Shift+Y\nrelink-media = Ctrl+R\n");
    REQUIRE(keymap);
    CHECK(keymap->shortcutFor("relink-media") == "Ctrl+R");
    // Carried through a save, for the same reason a project file carries its
    // unknown fields: an older build should not eat what a newer one wrote.
    CHECK(keymap->encode().find("from-the-future") != std::string::npos);
}

TEST_CASE("a keymap file that is not one is refused by line", "[keymap]") {
    CHECK_FALSE(ui::Keymap::decode("this is not a binding"));
    CHECK_FALSE(ui::Keymap::decode("relink-media = Ctrl+Wobble"));
    // Comments and blank lines are fine.
    CHECK(ui::Keymap::decode("# a comment\n\n   \nrelink-media = Ctrl+R\n"));
}

TEST_CASE("resetting all of it puts the catalogue back", "[keymap]") {
    ui::Keymap keymap;
    REQUIRE(keymap.setShortcut("relink-media", "Ctrl+Shift+R"));
    REQUIRE(keymap.clearShortcut("save-project"));
    keymap.resetAll();
    CHECK(keymap.shortcutFor("save-project") == "Ctrl+S");
    CHECK(keymap.shortcutFor("relink-media").empty());
}
