#include "Say.h"

#include <QMessageBox>
#include <cstdio>

namespace zaro::app {
namespace {

bool quiet = false;

void writeOut(const QString& title, const QString& text) {
    // One line per message, prefixed, so a long run's output can be read and
    // grepped rather than picked apart.
    QString flattened = text;
    flattened.replace('\n', QStringLiteral(" · "));
    std::fprintf(stderr, "zaro: %s: %s\n", title.toUtf8().constData(),
                 flattened.toUtf8().constData());
}

}  // namespace

void setQuiet(bool value) {
    quiet = value;
}

bool isQuiet() {
    return quiet;
}

void say(QWidget* parent, const QString& title, const QString& text) {
    if (quiet) {
        writeOut(title, text);
        return;
    }
    QMessageBox::information(parent, title, text);
}

void warn(QWidget* parent, const QString& title, const QString& text) {
    if (quiet) {
        writeOut(title, text);
        return;
    }
    QMessageBox::warning(parent, title, text);
}

}  // namespace zaro::app
