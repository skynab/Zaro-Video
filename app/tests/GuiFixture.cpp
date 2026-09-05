#include "GuiFixture.h"

#include <QApplication>
#include <QEventLoop>
#include <QMouseEvent>
#include <QSettings>
#include <QSize>
#include <QThread>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"
#include "zaro/core/model/Project.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"

#include "../Say.h"
#include "../Theme.h"

namespace zaro::app::testing {
namespace {

/// The fixture clip: 250 frames of black with flashes, and a click a second.
///
/// Chosen because so much of this suite measures a picture. A frame that is
/// black except where something was deliberately put makes "did this get
/// brighter" a real question, and the clicks give the audio tests a signal that
/// is mostly silence -- which is what a meter and a ducker are interesting on.
constexpr const char* kFixtureClip = "sync_click_flash.mov";

std::filesystem::path fixtureMedia() {
    return std::filesystem::path{ZARO_TESTDATA_DIR} / kFixtureClip;
}

/// Build the project the tests open: one clip, picture and sound, on V1 and A1.
///
/// The same shape `zaro-cut` produces, built here rather than by running that
/// tool so the suite has no dependency on another binary having been built.
Result<std::string> writeFixtureProject() {
    const std::string clipPath = fixtureMedia().string();
    auto probed = platform::ffmpeg::probe(clipPath);
    if (!probed) {
        return probed.error();
    }

    model::Project project;
    edit::CommandStack stack;

    model::MediaRef ref;
    ref.id = project.ids().next<model::MediaRefTag>();
    ref.path = clipPath;
    ref.name = kFixtureClip;
    ref.info = *probed;
    const model::MediaRefId mediaId = project.addMedia(ref);

    const media::VideoStreamInfo* video = probed->primaryVideo();
    const time::Rational rate = video != nullptr ? video->frameRate : time::rates::fps25;

    model::Sequence sequence{project.ids().next<model::SequenceTag>(), "Sequence 01", rate};
    sequence.setSize(video != nullptr ? video->width : 1920,
                     video != nullptr ? video->height : 1080);
    const auto sequenceId = sequence.id();
    const auto videoTrack = project.ids().next<model::TrackTag>();
    const auto audioTrack = project.ids().next<model::TrackTag>();
    sequence.addTrack(videoTrack, model::TrackKind::Video, "V1");
    sequence.addTrack(audioTrack, model::TrackKind::Audio, "A1");
    project.addSequence(std::move(sequence));

    const time::Rational sourceRate = video != nullptr ? video->frameRate : rate;
    const auto onTimeline = time::RationalTime::fromSeconds(probed->duration, rate);

    model::Clip clip;
    clip.id = project.ids().next<model::ClipTag>();
    clip.source = mediaId;
    clip.name = kFixtureClip;
    clip.sourceRange =
        time::TimeRange{time::RationalTime{0, sourceRate},
                        time::RationalTime::fromSeconds(probed->duration, sourceRate)};
    clip.timelineRange = time::TimeRange{time::RationalTime{0, rate}, onTimeline};

    auto placed = edit::makeOverwrite(project, {sequenceId, videoTrack}, clip);
    if (!placed) {
        return placed.error();
    }
    stack.execute(project, std::move(*placed));

    // Picture and sound from one file arrive together and stay together: the
    // razor test is about a cut reaching both, so a fixture whose two clips
    // merely line up would not be testing anything.
    model::Clip audioClip = clip;
    audioClip.id = project.ids().next<model::ClipTag>();
    auto placedAudio = edit::makeOverwrite(project, {sequenceId, audioTrack}, audioClip);
    if (!placedAudio) {
        return placedAudio.error();
    }
    stack.execute(project, std::move(*placedAudio));
    if (auto linked = edit::makeLinkClips(project, sequenceId,
                                          {{videoTrack, clip.id}, {audioTrack, audioClip.id}})) {
        stack.execute(project, std::move(*linked));
    }

    project.setActiveSequence(sequenceId);
    const std::string path =
        (std::filesystem::path{ZARO_SCRATCH_DIR} / "gui_fixture.zaro").string();
    if (auto saved = io::saveProject(project, path); !saved) {
        return saved.error();
    }
    return path;
}

/// Held for the life of the process: Qt objects must outlive every test, and
/// tearing a GPU context down between tests is what this suite is avoiding.
struct Holder {
    int argc{1};
    std::vector<char*> argv;
    std::string program{"zaro_app_tests"};
    std::unique_ptr<QApplication> application;
    std::unique_ptr<PreviewWindow> window;
    std::string fixturePath;
    model::SequenceId baseSequence;
    bool ready{false};
};

Holder& holder() {
    static Holder instance;
    return instance;
}

}  // namespace

std::string mediaFixture(const char* name) {
    return (std::filesystem::path{ZARO_TESTDATA_DIR} / name).string();
}

double meanGray(const QImage& image) {
    if (image.isNull()) {
        return 0.0;
    }
    double total = 0.0;
    for (int yy = 0; yy < image.height(); ++yy) {
        for (int xx = 0; xx < image.width(); ++xx) {
            total += qGray(image.pixel(xx, yy));
        }
    }
    return total / (image.width() * image.height());
}

// The messages come from the call sites as literals; the warning is about
// this function forwarding one it cannot see.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
void failf(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    // The messages were written to be read on a terminal and end in a newline;
    // Catch2 adds its own framing, so the newline would leave a blank line in
    // the middle of the report.
    std::string message{buffer};
    while (!message.empty() && (message.back() == '\n' || message.back() == ' ')) {
        message.pop_back();
    }
    FAIL(message);
    // FAIL throws, but the compiler cannot see that through the macro.
    std::abort();
}
#pragma GCC diagnostic pop

Rewind::~Rewind() {
    Holder& held = holder();
    if (!held.ready) {
        return;
    }
    PreviewWindow& window = *held.window;
    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.renderCache().clear();
    QApplication::processEvents();
}

void discard(const std::filesystem::path& path) {
    std::error_code code;
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::filesystem::remove_all(path, code);
        if (!std::filesystem::exists(path, code)) {
            return;
        }
        // Pumping the loop between attempts is not just a wait: a thumbnail
        // decode this test queued finishes on its worker and lets go of the
        // file, and the answer it posts back is delivered here.
        QApplication::processEvents();
        QThread::msleep(50);
    }
    std::fprintf(stderr, "  note: %s could not be deleted and was left behind\n",
                 path.string().c_str());
}

void moveAside(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code code;
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::filesystem::rename(from, to, code);
        if (!code) {
            return;
        }
        QApplication::processEvents();
        QThread::msleep(50);
    }
    failf("could not move %s: %s\n", from.string().c_str(), code.message().c_str());
}

void restoreFixtureProject() {
    Holder& held = holder();
    if (!held.ready) {
        return;
    }
    PreviewWindow& window = *held.window;
    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    // Reloaded from the file, every test, unconditionally.
    //
    // Undo covers the edits that went through the command stack, and a good
    // deal of what these tests do does not: setting a transfer override, giving
    // a media reference a proxy, pointing one at a file that moved. Those are
    // written straight onto the project, so the stack has nothing to unwind and
    // the next test inherits them. What that looked like was a media reference
    // still pointing where a relink test had left it, failing a test three
    // files away with a path that does not exist. Cheaper to be sure than to
    // keep finding the exceptions.
    auto loaded = io::loadProject(held.fixturePath);
    if (!loaded) {
        FAIL("could not reload the fixture project: " + loaded.error().toString());
    }
    model::Project project = loaded->project;
    window.adopt(std::move(project), std::move(*loaded), held.fixturePath);

    window.setWorkspace("Edit");
    // The tool and the snapping switch are chrome, not project state, so undo
    // and a reload both leave them exactly where the last test put them. A
    // razor test that ends with the blade selected makes the next test's drag
    // a cut, which it reports as a trim that did not trim -- and which of the
    // two failed depended on the order Catch2 happened to run them in.
    window.timeline()->setTool(app::TimelineWidget::Tool::Select);
    // Row heights are chrome in the same way, and they are per row: a test that
    // made one track tall -- or that threw partway through a drag on its edge,
    // leaving the row it had grown -- hands the next test a panel laid out
    // differently from the one it measured, and the failure lands over there.
    window.timeline()->setTrackHeightScale(1.0);
    window.timeline()->setTrackHeight(model::TrackKind::Video,
                                      app::TimelineWidget::kDefaultVideoTrackHeight);
    window.timeline()->setTrackHeight(model::TrackKind::Audio,
                                      app::TimelineWidget::kDefaultAudioTrackHeight);
    // Take the deferred fit now rather than whenever the next resize arrives.
    //
    // Binding a project asks the timeline to zoom to fit, and it does that on
    // its next resizeEvent. Under a window manager that event can turn up after
    // a test has already asked the layout where a clip's edge is, so the press
    // goes to a pixel the widget has since stopped agreeing with -- which is a
    // trim that quietly does not trim, about one run in six. Doing it here
    // means every test starts from the same zoom, and a known one.
    window.timeline()->zoomToFit();
    window.timeline()->setSnapEnabled(true);
    // End any gesture still in progress.
    //
    // A drag is press, move, release, and the widget keeps state between them.
    // A test that pressed and did not release -- or one whose assertion threw
    // between the two -- leaves that state set, and the next press is then
    // treated as part of a gesture that started somewhere else. That is a trim
    // that does nothing, with geometry that checks out and a hit test that
    // agrees, which is exactly how it presented.
    {
        QMouseEvent release{QEvent::MouseButtonRelease,
                            QPointF{0, 0},
                            QPointF{0, 0},
                            Qt::LeftButton,
                            Qt::NoButton,
                            Qt::NoModifier};
        QCoreApplication::sendEvent(window.timeline(), &release);
    }
    // Back to the head, before the settle below measures anything.
    //
    // The timeline scrolls to keep the playhead in view, so a test that left it
    // at the end of the clip leaves the next one looking at a scrolled view --
    // and the x it computes for a clip's edge is then measured from an origin
    // that moves again on the next repaint. That is what made a trim drag miss
    // the edge it was aimed at, about one run in three.
    if (const model::Sequence* seq = window.sequence(); seq != nullptr) {
        window.setPosition(time::RationalTime{0, seq->frameRate()});
    }
    window.renderCache().clear();

    // Settle before handing the window over.
    //
    // Adopting a project re-binds every panel, and the timeline only knows
    // where a clip is once it has been laid out at its real width. A test that
    // asks xForTime too early gets coordinates from a widget that is still
    // nominally narrow, drives the mouse somewhere that is not the clip's edge,
    // and reports that trimming does not trim or that the blade missed.
    //
    // "Has a width" is not enough: the geometry arrives in more than one step,
    // and three fast polls can all land inside the same intermediate one. What
    // is waited for is the whole geometry a gesture depends on -- the widget's
    // width, the row's top and height, and where the clip's out point falls --
    // unchanged across five samples a frame apart. Bounded, so a window that
    // never settles fails the test rather than hanging the run.
    const model::Sequence* sequence = window.sequence();
    std::array<double, 4> last{-1.0, -1.0, -1.0, -1.0};
    int steady = 0;
    for (int spin = 0; spin < 200 && steady < 5; ++spin) {
        QApplication::processEvents(QEventLoop::AllEvents, 16);
        if (sequence == nullptr || sequence->videoTracks().empty()) {
            steady = 0;
            continue;
        }
        const auto& track = sequence->videoTracks().front();
        if (track.clips().empty()) {
            steady = 0;
            continue;
        }
        const auto row = window.timeline()->rowFor(track.id());
        if (!row || row->height <= 0 || window.timeline()->width() <= 1) {
            steady = 0;
            continue;
        }
        const std::array<double, 4> now{
            static_cast<double>(window.timeline()->width()), static_cast<double>(row->top),
            static_cast<double>(row->height),
            window.timeline()->layout().xForTime(track.clips().front().endExclusive())};
        if (now[3] <= 1.0) {
            steady = 0;
            last = now;
            continue;
        }
        steady = now == last ? steady + 1 : 0;
        last = now;
    }

    window.monitor()->update();
    QApplication::processEvents();
}

PreviewWindow& gui() {
    Holder& held = holder();
    if (held.ready) {
        restoreFixtureProject();
        return *held.window;
    }

    if (!std::filesystem::exists(fixtureMedia())) {
        SKIP("no media fixtures: run ./testdata/generate.sh");
    }

    held.argv = {held.program.data(), nullptr};
    held.application = std::make_unique<QApplication>(held.argc, held.argv.data());
    theme::apply(*held.application);
    // The hotkeys test rebinds a key and writes the keymap out. Left at its
    // default that is the developer's own keymap.conf, so running the suite
    // would rebind Save in the editor they use -- and a stale one from an
    // earlier run comes back the next time and fails the test that wrote it.
    const QString keymap =
        QString::fromStdString((std::filesystem::path{ZARO_SCRATCH_DIR} / "keymap.conf").string());
    std::filesystem::remove(keymap.toStdString());
    PreviewWindow::setKeymapPath(keymap);
    // Settings of their own, for the same reason and one step further.
    // `PreviewWindow::restoreWorkspace` reads the application's own settings
    // and restores the window geometry, the splitter positions and the
    // workspace that was last open -- so the suite came up at whatever size
    // and in whatever workspace the developer last left the app in. That is
    // not cosmetic: the timeline's pixels-per-second follows the window width,
    // so a ten-pixel snap radius is a different number of frames on a
    // maximised window than on CI's, and the Color workspace has no timeline
    // at all. Set before the window is built, which is what reads them.
    const QString settings =
        QString::fromStdString((std::filesystem::path{ZARO_SCRATCH_DIR} / "settings.ini").string());
    std::filesystem::remove(settings.toStdString());
    PreviewWindow::setSettingsPath(settings);
    // Anything that would have opened a dialog says so on stderr instead. A
    // modal dialog in a test is a hang, and the run has no one to close it.
    setQuiet(true);
    platform::ffmpeg::installLogHandler(false);

    auto path = writeFixtureProject();
    if (!path) {
        FAIL("could not build the fixture project: " + path.error().toString());
    }
    auto loaded = io::loadProject(*path);
    if (!loaded) {
        FAIL("could not load the fixture project: " + loaded.error().toString());
    }
    model::Project project = loaded->project;
    held.window = std::make_unique<PreviewWindow>(std::move(project), std::move(*loaded), *path);
    if (auto opened = held.window->openMedia(); !opened) {
        FAIL("could not open the fixture media: " + opened.error().toString());
    }
    held.window->resize(960, 620);
    held.window->show();
    // Wait for the window to stop being resized before anything measures it.
    //
    // A window that has just been shown is not yet the size it will settle at:
    // a window manager gives it one, and how long that takes is not ours to
    // know. Every gesture in this suite is aimed at a pixel computed from the
    // timeline's width, so the first test to run was the one that paid for it.
    QSize lastSize;
    int settled = 0;
    for (int spin = 0; spin < 300 && settled < 10; ++spin) {
        QApplication::processEvents(QEventLoop::AllEvents, 16);
        const QSize now = held.window->size();
        settled = now == lastSize ? settled + 1 : 0;
        lastSize = now;
    }

    PreviewWindow& window = *held.window;
    REQUIRE(window.sequence() != nullptr);
    REQUIRE_FALSE(window.sequence()->videoTracks().empty());
    REQUIRE_FALSE(window.sequence()->videoTracks().front().clips().empty());

    held.fixturePath = *path;
    held.baseSequence = window.sequence()->id();
    held.ready = true;
    // Through the same door as every other test. A freshly shown window has not
    // laid its timeline out either, and the first test to run was reading
    // coordinates from it before it had -- which made whichever test Catch2
    // happened to start with fail about one run in four.
    restoreFixtureProject();
    return window;
}

namespace {

/// Take the window down before the process does.
///
/// Left to static destruction the window outlives Qt's own statics, and what
/// that looks like is a segfault after the results have already been printed --
/// a run that reports every test passing and still exits non-zero.
struct Teardown : Catch::EventListenerBase {
    using Catch::EventListenerBase::EventListenerBase;

    void testRunEnded(const Catch::TestRunStats& /*stats*/) override {
        Holder& held = holder();
        held.ready = false;
        held.window.reset();
        held.application.reset();
    }
};

CATCH_REGISTER_LISTENER(Teardown)

}  // namespace

}  // namespace zaro::app::testing
