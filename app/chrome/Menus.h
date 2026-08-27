// The menu bar: which commands appear where.
#pragma once

#include <QAction>
#include <QMap>
#include <QMenuBar>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <functional>

#include "../ActionRouter.h"

namespace zaro::app::chrome {

/// Build the menu bar.
///
/// `addWindowMenus` is called with the finished bar to append the menus that
/// are about one particular window rather than about the program: which of its
/// panels are showing, and what it says about itself.
QMenuBar* buildMenuBar(QWidget* parent, ActionRouter& router, const QStringList& workspaces,
                       QMap<QString, QAction*>& workspaceActions,
                       const std::function<void(const QString&)>& chooseWorkspace,
                       const std::function<void(QMenuBar*)>& addWindowMenus);

}  // namespace zaro::app::chrome
