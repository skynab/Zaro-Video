#include "QtMessageLog.h"

#include <QString>
#include <QtGlobal>
#include <mutex>
#include <string>
#include <vector>

namespace zaro::testing {
namespace {

std::mutex& guard() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::string>& collected() {
    static std::vector<std::string> messages;
    return messages;
}

QtMessageHandler previousHandler = nullptr;

void collect(QtMsgType type, const QMessageLogContext& context, const QString& text) {
    const char* label = "";
    switch (type) {
        case QtWarningMsg:
            label = "warning: ";
            break;
        case QtCriticalMsg:
            label = "critical: ";
            break;
        case QtFatalMsg:
            label = "fatal: ";
            break;
        default:
            break;  // debug and info are noise here
    }
    if (label[0] != '\0') {
        const std::lock_guard<std::mutex> lock{guard()};
        collected().push_back(label + text.toStdString());
    }
    // Passed on rather than swallowed: a test run watched by a person should
    // still print what Qt had to say, when and where Qt said it.
    if (previousHandler != nullptr) {
        previousHandler(type, context, text);
    }
}

/// Installed before main, because the first thing worth hearing about can
/// happen in the first line of the first test.
struct Installer {
    Installer() { previousHandler = qInstallMessageHandler(collect); }
};
const Installer installer;

}  // namespace

std::string takeQtMessages() {
    std::vector<std::string> taken;
    {
        const std::lock_guard<std::mutex> lock{guard()};
        taken.swap(collected());
    }
    std::string joined;
    for (const std::string& message : taken) {
        joined += "\n  Qt " + message;
    }
    return joined;
}

}  // namespace zaro::testing
