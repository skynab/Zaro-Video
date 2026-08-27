#include "ActionRouter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QWidget>
#include <cstdio>
#include <utility>

#include "zaro/core/Check.h"
#include "zaro/ui/Actions.h"

namespace zaro::app {

ActionRouter::ActionRouter(QWidget* owner) : QObject{owner}, owner_{owner} {}

void ActionRouter::bind(const char* actionId, std::function<void()> handler) {
    ZARO_CHECK(ui::findAction(actionId) != nullptr, "a binding names an action nobody catalogued");
    handlers_.insert_or_assign(actionId, [run = std::move(handler)](bool) { run(); });
}

void ActionRouter::bindToggle(const char* actionId, std::function<void(bool)> handler) {
    ZARO_CHECK(ui::findAction(actionId) != nullptr, "a binding names an action nobody catalogued");
    handlers_.insert_or_assign(actionId, std::move(handler));
    toggles_.insert_or_assign(actionId, true);
}

bool ActionRouter::trigger(const std::string& actionId) {
    const auto handler = handlers_.find(actionId);
    if (handler == handlers_.end()) {
        return false;
    }
    if (toggles_.find(actionId) == toggles_.end()) {
        handler->second(false);
        return true;
    }
    // A toggle reached by keystroke rather than by clicking its menu item has
    // no "on" to be told, so it flips whatever its action currently says.
    QAction* action = find(QString::fromStdString(actionId));
    const bool wanted = action != nullptr ? !action->isChecked() : true;
    if (action != nullptr) {
        action->setChecked(wanted);
    }
    handler->second(wanted);
    return true;
}

QAction* ActionRouter::action(const char* actionId) {
    const QString key = QString::fromUtf8(actionId);
    if (QAction* existing = actions_.value(key, nullptr)) {
        return existing;
    }
    const ui::ActionInfo* info = ui::findAction(actionId);
    ZARO_CHECK(info != nullptr, "a menu item names an action nobody catalogued");

    auto* made = new QAction(
        QString::fromUtf8(info->label.data(), static_cast<int>(info->label.size())), this);
    // The id is the object name, so the tests and anything else that reaches
    // for a command by name uses the same name the keymap does.
    made->setObjectName(key);
    const std::string id{actionId};
    if (toggles_.find(id) != toggles_.end()) {
        made->setCheckable(true);
        connect(made, &QAction::triggered, this, [this, id](bool on) {
            const auto handler = handlers_.find(id);
            if (handler != handlers_.end()) {
                handler->second(on);
            }
        });
    } else {
        connect(made, &QAction::triggered, this, [this, id] { trigger(id); });
    }
    actions_.insert(key, made);
    applyShortcut(key, made);
    return made;
}

QAction* ActionRouter::find(const QString& actionId) const {
    return actions_.value(actionId, nullptr);
}

void ActionRouter::applyShortcut(const QString& actionId, QAction* action) {
    const std::string shortcut = keymap_.shortcutFor(actionId.toStdString());
    // A bare letter is not made a Qt shortcut: it would fire while somebody is
    // typing into a field. Those reach their command through the key handler
    // instead, which is why every command is in the one registry whether or not
    // it can carry a shortcut.
    const bool hasModifier = shortcut.find('+') != std::string::npos;
    if (shortcut.empty() || !hasModifier) {
        action->setShortcut(QKeySequence{});
        return;
    }
    action->setShortcut(
        QKeySequence::fromString(QString::fromStdString(shortcut), QKeySequence::PortableText));
    action->setShortcutContext(Qt::WindowShortcut);
}

void ActionRouter::applyKeymap() {
    for (auto entry = actions_.constBegin(); entry != actions_.constEnd(); ++entry) {
        applyShortcut(entry.key(), entry.value());
    }
}

QString ActionRouter::keymapPath() {
    if (!keymapPath_.isEmpty()) {
        return keymapPath_;
    }
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return folder + "/keymap.conf";
}

void ActionRouter::setKeymapPath(const QString& path) {
    keymapPath_ = path;
}

void ActionRouter::loadKeymap() {
    QFile file{keymapPath()};
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;  // nobody has customised anything, which is the usual case
    }
    auto loaded = ui::Keymap::decode(QString::fromUtf8(file.readAll()).toStdString());
    if (!loaded) {
        // A keymap that will not parse leaves the defaults in place rather than
        // stopping the application: somebody who hand-edited it into a mess
        // should still be able to start the program and fix it.
        std::fprintf(stderr, "zaro: %s\n", loaded.error().toString().c_str());
        return;
    }
    keymap_ = std::move(*loaded);
}

void ActionRouter::saveKeymap() {
    const QString path = keymapPath();
    QDir{}.mkpath(QFileInfo{path}.absolutePath());
    QFile file{path};
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        return;
    }
    file.write(QByteArray::fromStdString(keymap_.encode()));
}

}  // namespace zaro::app
