#pragma once

#include <QDialog>
#include <functional>
#include <string>

#include "zaro/ui/Keymap.h"

class QLabel;
class QPushButton;
class QTableWidget;

namespace zaro::app {

/// Change what the keys do.
///
/// The window is deliberately plain: a table of every command with what it is
/// bound to, a button that listens for the next keystroke, and one that puts a
/// binding back. What matters here is underneath -- the catalogue of actions
/// and the keymap -- and this is the smallest thing that lets somebody use it.
///
/// It edits the window's keymap directly and calls back when anything changes,
/// so what is shown and what the keys do cannot drift apart.
class Hotkeys : public QDialog {
    Q_OBJECT

public:
    explicit Hotkeys(ui::Keymap& keymap, QWidget* parent = nullptr);

    /// Called after every change, to re-apply the bindings and save them.
    void setOnChanged(std::function<void()> handler) { onChanged_ = std::move(handler); }

    void refresh();

    /// Bind the selected action, as though somebody had pressed these keys.
    /// Public because it is the action, and the capture button is one way of
    /// asking for it.
    [[nodiscard]] Status assign(const std::string& actionId, const std::string& keystroke);
    [[nodiscard]] Status resetOne(const std::string& actionId);
    void selectAction(const std::string& actionId);
    [[nodiscard]] std::string selectedAction() const;

protected:
    /// While recording, every key press is the binding rather than a shortcut.
    void keyPressEvent(QKeyEvent* event) override;

private:
    void changed();

    ui::Keymap& keymap_;
    std::function<void()> onChanged_;
    QTableWidget* table_{nullptr};
    QLabel* footer_{nullptr};
    QPushButton* record_{nullptr};
    bool recording_{false};
};

}  // namespace zaro::app
