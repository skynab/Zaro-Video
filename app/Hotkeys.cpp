#include "Hotkeys.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "zaro/ui/Actions.h"

namespace zaro::app {
namespace {

QString text(std::string_view view) {
    return QString::fromUtf8(view.data(), static_cast<int>(view.size()));
}

}  // namespace

Hotkeys::Hotkeys(ui::Keymap& keymap, QWidget* parent) : QDialog{parent}, keymap_{keymap} {
    setWindowTitle("Keyboard Shortcuts");
    resize(560, 520);

    table_ = new QTableWidget(this);
    table_->setObjectName("hotkey-table");
    table_->setColumnCount(3);
    table_->setHorizontalHeaderLabels({"Command", "Where", "Shortcut"});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);

    record_ = new QPushButton("Press a Key…", this);
    record_->setObjectName("hotkey-record");
    record_->setCheckable(true);
    record_->setToolTip("Then press the keys you want for the selected command");
    auto* clear = new QPushButton("Clear", this);
    clear->setObjectName("hotkey-clear");
    auto* reset = new QPushButton("Reset", this);
    reset->setObjectName("hotkey-reset");
    auto* resetAll = new QPushButton("Reset All", this);
    resetAll->setObjectName("hotkey-reset-all");
    auto* close = new QPushButton("Close", this);

    footer_ = new QLabel(this);
    footer_->setProperty("muted", true);
    footer_->setWordWrap(true);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(record_);
    buttons->addWidget(clear);
    buttons->addWidget(reset);
    buttons->addWidget(resetAll);
    buttons->addStretch(1);
    buttons->addWidget(close);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(table_, 1);
    layout->addWidget(footer_);
    layout->addLayout(buttons);

    connect(record_, &QPushButton::toggled, this, [this](bool on) {
        recording_ = on;
        footer_->setText(on ? "Listening — press the keys you want, or Escape to stop."
                            : QString{});
        if (on) {
            // The keys have to reach this window rather than the table, which
            // would take the arrows for itself.
            setFocus(Qt::OtherFocusReason);
        }
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        const std::string chosen = selectedAction();
        if (chosen.empty()) {
            return;
        }
        static_cast<void>(keymap_.clearShortcut(chosen));
        changed();
    });
    connect(reset, &QPushButton::clicked, this, [this] {
        if (const std::string chosen = selectedAction(); !chosen.empty()) {
            static_cast<void>(resetOne(chosen));
        }
    });
    connect(resetAll, &QPushButton::clicked, this, [this] {
        keymap_.resetAll();
        changed();
    });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    refresh();
}

void Hotkeys::refresh() {
    const std::string keep = selectedAction();
    table_->setRowCount(static_cast<int>(ui::allActions().size()));
    int row = 0;
    for (const ui::ActionInfo& action : ui::allActions()) {
        auto* label = new QTableWidgetItem(text(action.label));
        label->setData(Qt::UserRole, text(action.id));
        table_->setItem(row, 0, label);
        table_->setItem(row, 1, new QTableWidgetItem(text(action.category)));

        const std::string shortcut = keymap_.shortcutFor(action.id);
        auto* bound = new QTableWidgetItem(QString::fromStdString(shortcut));
        if (!keymap_.isDefault(action.id)) {
            // Marked, because "what have I changed" is the first question
            // somebody asks of a keymap they have been editing for a month.
            QFont font = bound->font();
            font.setBold(true);
            bound->setFont(font);
        }
        table_->setItem(row, 2, bound);
        ++row;
    }
    table_->resizeColumnsToContents();
    if (!keep.empty()) {
        selectAction(keep);
    } else if (table_->rowCount() > 0) {
        table_->selectRow(0);
    }
}

std::string Hotkeys::selectedAction() const {
    const int row = table_->currentRow();
    if (row < 0 || table_->item(row, 0) == nullptr) {
        return {};
    }
    return table_->item(row, 0)->data(Qt::UserRole).toString().toStdString();
}

void Hotkeys::selectAction(const std::string& actionId) {
    for (int row = 0; row < table_->rowCount(); ++row) {
        if (table_->item(row, 0)->data(Qt::UserRole).toString().toStdString() == actionId) {
            table_->selectRow(row);
            return;
        }
    }
}

Status Hotkeys::assign(const std::string& actionId, const std::string& keystroke) {
    if (Status bound = keymap_.setShortcut(actionId, keystroke); !bound) {
        // Said, not swallowed: the message names the command already holding
        // it, which is what somebody needs to decide what to do next.
        footer_->setText(QString::fromStdString(bound.error().message()));
        return bound;
    }
    changed();
    return {};
}

Status Hotkeys::resetOne(const std::string& actionId) {
    if (Status back = keymap_.resetToDefault(actionId); !back) {
        return back;
    }
    changed();
    return {};
}

void Hotkeys::changed() {
    refresh();
    if (onChanged_) {
        onChanged_();
    }
}

void Hotkeys::keyPressEvent(QKeyEvent* event) {
    if (!recording_) {
        QDialog::keyPressEvent(event);
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        record_->setChecked(false);
        return;
    }
    switch (event->key()) {
        case Qt::Key_Control:
        case Qt::Key_Shift:
        case Qt::Key_Alt:
        case Qt::Key_Meta:
            // Still waiting: somebody holding Ctrl on the way to Ctrl+S has not
            // chosen anything yet.
            return;
        default:
            break;
    }
    const std::string chosen = selectedAction();
    if (chosen.empty()) {
        return;
    }
    const QKeySequence sequence{event->keyCombination()};
    auto normalised =
        ui::normaliseShortcut(sequence.toString(QKeySequence::PortableText).toStdString());
    if (!normalised) {
        footer_->setText(QString::fromStdString(normalised.error().message()));
        return;
    }
    record_->setChecked(false);
    static_cast<void>(assign(chosen, *normalised));
}

}  // namespace zaro::app
