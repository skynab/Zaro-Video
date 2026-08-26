#pragma once

#include <QString>

class QWidget;

namespace zaro::app {

/// Whether the application is allowed to stop and ask.
///
/// **Because a modal dialog is a blocked machine.** Everything this program
/// says in passing -- what a relink found, how long a remix came out, that a
/// project is read-only -- arrives as a box somebody has to dismiss. That is
/// reasonable when a person is sitting there and intolerable when one is not:
/// a run under a script, a self-test, or somebody iterating on the code stops
/// dead on a sentence nobody needed to read.
///
/// Quiet mode says the same words on stderr instead. Nothing is hidden; it is
/// the waiting that goes. Turned on by `--quiet`, by `ZARO_QUIET=1`, and by
/// every self-test.
void setQuiet(bool quiet);
[[nodiscard]] bool isQuiet();

/// Something worth knowing. A dialog, or a line on stderr when quiet.
void say(QWidget* parent, const QString& title, const QString& text);

/// Something that went wrong. Same rule: the words are not the problem, the
/// waiting is.
void warn(QWidget* parent, const QString& title, const QString& text);

}  // namespace zaro::app
