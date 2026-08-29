// zaro-preview: the application entry point.
//
// The window itself is PreviewWindow, in PreviewWindow.h. What is left here is
// what a main is for: read the arguments, load the project, show the window,
// and the two small smoke checks that are worth being able to run against the
// shipping binary. The rest of the GUI tests live in app/tests.

#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QPixmap>
#include <QStringList>
#include <QSysInfo>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include "zaro/core/Environment.h"

#include "FrameGrab.h"
#include "PreviewWindow.h"
#include "Say.h"
#include "Theme.h"

using namespace zaro;
using zaro::app::dragOnTimeline;
using zaro::app::PreviewWindow;
using zaro::app::settledGrab;

int main(int argc, char** argv) {
    // Line buffered, always. The self-tests print as they go and are usually
    // read from a redirected file; fully buffered, that file stays empty until
    // the process exits, so a run that hangs looks exactly like a run that has
    // not started -- which cost an hour of looking in the wrong place once.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    QApplication application(argc, argv);
    // Before any window exists: the palette and the sheet decide what every
    // widget looks like the moment it is constructed, and a window built first
    // flashes the platform's own colours on its way to these.
    zaro::app::theme::apply(application);

    QStringList arguments = QApplication::arguments();
    const bool selfTest = arguments.removeAll("--selftest") > 0;
    const bool editTest = arguments.removeAll("--selftest-edit") > 0;
    const bool quitTest = arguments.removeAll("--selftest-quit") > 0;
    // Quiet: say things on stderr rather than in a box somebody has to
    // dismiss. On for the self-tests always, because a test that stops for a
    // dialog is a test that hangs.
    const auto quietWanted = zaro::environmentValue("ZARO_QUIET");
    PreviewWindow::setQuietMode(arguments.removeAll("--quiet") > 0 ||
                                (quietWanted.has_value() && quietWanted->starts_with('1')) ||
                                selfTest || editTest || quitTest);

    // A keymap of its own for the self-tests, and for anybody who wants one
    // beside a project: the tests rebind commands, and rebinding somebody's
    // real Save key because they ran a test is not on.
    if (const auto wanted = zaro::environmentValue("ZARO_KEYMAP"); wanted.has_value()) {
        PreviewWindow::setKeymapPath(QString::fromStdString(*wanted));
    } else if (selfTest || editTest || quitTest) {
        PreviewWindow::setKeymapPath(QDir::temp().filePath("zaro-selftest-keymap.conf"));
    }

    // Project locking is off unless asked for. See PreviewWindow::lockingEnabled.
    const auto lockingWanted = zaro::environmentValue("ZARO_LOCKING");
    PreviewWindow::setLockingEnabled(
        arguments.removeAll("--locking") > 0 ||
        (lockingWanted.has_value() && lockingWanted->starts_with('1')));
    QString capturePath;
    if (const auto at = arguments.indexOf("--capture"); at >= 0 && at + 1 < arguments.size()) {
        capturePath = arguments.at(at + 1);
        arguments.removeAt(at + 1);
        arguments.removeAt(at);
    }
    if (arguments.size() < 2) {
        std::puts("usage: zaro-preview <project.zaro> [--selftest]");
        std::puts("");
        std::puts("  space        play / pause        J K L   shuttle");
        std::puts("  left/right   step one frame      home/end  start / end");
        std::puts("");
        std::puts("  --selftest        render, verify a picture came out, exit");
        std::puts("  --capture <png>   with --selftest, save what the monitor showed");
        std::puts("  --selftest-edit   moved: see the zaro_app_tests target");
        std::puts("  --selftest-quit   quit with background work in flight, exit");
        std::puts("  --locking         take a lock on the project, and honour other people's");
        std::puts("  --quiet           say things on stderr instead of in dialogs");
        return 2;
    }

    zaro::platform::ffmpeg::installLogHandler(false);

    const std::string projectPath = arguments.at(1).toStdString();

    // A recovery file newer than the project means the last session ended
    // without an explicit save. Offered rather than opened: the autosave is
    // what somebody was in the middle of, and only they know whether they want
    // it. Declining leaves the file alone, so the choice can be made again.
    std::string openPath = projectPath;
    if (!quitTest && !selfTest && !editTest && zaro::io::hasNewerAutosave(projectPath)) {
        const auto answer = QMessageBox::question(
            nullptr, "Recover",
            QString("There is a more recent recovery file for %1.\n\nOpen it instead?")
                .arg(QString::fromStdString(projectPath)));
        if (answer == QMessageBox::Yes) {
            openPath = zaro::io::autosavePath(projectPath);
        }
    }

    auto loaded = zaro::io::loadProject(openPath);
    if (!loaded) {
        std::fprintf(stderr, "zaro-preview: %s\n", loaded.error().toString().c_str());
        return 1;
    }
    zaro::model::Project project = loaded->project;
    if (project.findSequence(project.activeSequence()) == nullptr) {
        std::fprintf(stderr, "zaro-preview: this project has no active sequence\n");
        return 1;
    }

    // Recovered work is saved back to the *project*, not to the recovery file:
    // opening an autosave and then saving into it would leave the real project
    // stale for ever.
    PreviewWindow window{std::move(project), std::move(*loaded), projectPath};
    if (const auto status = window.openMedia(); !status) {
        std::fprintf(stderr, "zaro-preview: %s\n", status.error().toString().c_str());
        return 1;
    }
    window.resize(960, 620);
    window.show();

    if (quitTest) {
        // Quit while the waveform thread is still running, which is what
        // happens when someone presses Cmd+Q on a freshly opened project.
        // The window is a local here, so returning destroys it -- and a
        // std::thread destroyed while still joinable calls std::terminate.
        // No joining, no waiting: that is the point.
        QApplication::processEvents();
        std::printf("zaro-preview quit selftest: exiting with background work in flight\n");
        return 0;
    }

    if (editTest) {
        // The GUI tests moved to app/tests, where they run as sixty separate
        // Catch2 cases against the same window this builds. They were 6,358
        // lines inside this function, which meant nothing ran them: there was
        // no ctest entry and no CI job, and the first failure returned out of
        // main with the other fifty-nine sections never reached.
        std::fprintf(stderr,
                     "zaro-preview: --selftest-edit is now the zaro_app_tests target.\n"
                     "               ctest -R zaro_app_tests\n");
        return 2;
    }

    if (!selfTest) {
        return QApplication::exec();
    }

    // Prove a picture actually reached the widget, rather than that the window
    // opened. Renders a handful of frames across the sequence and reads one
    // back -- the only readback in this program, and it exists for this check.
    const zaro::model::Sequence& sequence = *window.sequence();
    const std::int64_t last = std::max<std::int64_t>(0, sequence.duration().frames() - 1);
    window.waitForWaveforms();
    // Sample across the sequence and keep the brightest. A single position is
    // not a fair test: plenty of real footage is legitimately black at any
    // given moment, and a fixture that is black except on flash frames would
    // fail a check aimed at one timecode.
    QImage grabbed;
    double bestLit = 0.0;
    for (int i = 0; i < 5; ++i) {
        window.setPosition(zaro::time::RationalTime{last * i / 5, sequence.frameRate()});
        QApplication::processEvents();
        const QImage shot = window.monitor()->grabFramebuffer();
        if (shot.isNull()) {
            continue;
        }
        std::int64_t lit = 0;
        for (int y = 0; y < shot.height(); ++y) {
            for (int x = 0; x < shot.width(); ++x) {
                if (qGray(shot.pixel(x, y)) > 8) {
                    ++lit;
                }
            }
        }
        const double fraction =
            static_cast<double>(lit) / static_cast<double>(shot.width() * shot.height());
        if (grabbed.isNull() || fraction > bestLit) {
            bestLit = fraction;
            grabbed = shot;
        }
    }

    if (!window.monitor()->lastError().isEmpty()) {
        std::fprintf(stderr, "zaro-preview: %s\n",
                     window.monitor()->lastError().toUtf8().constData());
        return 1;
    }
    if (grabbed.isNull()) {
        std::fprintf(stderr, "zaro-preview: the monitor produced no image\n");
        return 1;
    }

    // Black at every sampled position would mean the pipeline ran and drew
    // nothing, which is the failure this is really looking for.
    const double litFraction = bestLit;

    std::printf("zaro-preview selftest\n");
    std::printf("  %lld frames rendered through the widget\n",
                static_cast<long long>(window.monitor()->framesRendered()));
    std::printf("  grabbed %dx%d, %.1f%% of it lit\n", grabbed.width(), grabbed.height(),
                litFraction * 100.0);

    if (!capturePath.isEmpty()) {
        // The whole window, so the timeline is in the picture too.
        const QPixmap windowShot = window.grab();
        if (!windowShot.isNull()) {
            windowShot.save(capturePath + ".window.png");
        }
        if (grabbed.save(capturePath)) {
            std::printf("  saved %s\n", capturePath.toUtf8().constData());
        } else {
            std::fprintf(stderr, "  FAIL: cannot write %s\n", capturePath.toUtf8().constData());
            return 1;
        }
    }

    if (litFraction < 0.05) {
        std::fprintf(stderr, "  FAIL: the monitor is essentially black\n");
        return 1;
    }
    std::printf("  ok\n");
    return 0;
}
