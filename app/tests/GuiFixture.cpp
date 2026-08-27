#include "GuiFixture.h"

#include <QApplication>
#include <cstdarg>
#include <cstdio>
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

void restoreFixtureProject() {
    Holder& held = holder();
    if (!held.ready) {
        return;
    }
    PreviewWindow& window = *held.window;
    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    // Reloaded outright rather than unpicked. Undo covers the edits; it does
    // not cover which sequence is active, a project somebody opened instead, or
    // the read-only flag a lock left set -- and a test that inherits any of
    // those fails for a reason that has nothing to do with what it is testing.
    auto loaded = io::loadProject(held.fixturePath);
    if (!loaded) {
        FAIL("could not reload the fixture project: " + loaded.error().toString());
    }
    model::Project project = loaded->project;
    window.adopt(std::move(project), std::move(*loaded), held.fixturePath);
    window.renderCache().clear();
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
    QApplication::processEvents();

    PreviewWindow& window = *held.window;
    REQUIRE(window.sequence() != nullptr);
    REQUIRE_FALSE(window.sequence()->videoTracks().empty());
    REQUIRE_FALSE(window.sequence()->videoTracks().front().clips().empty());

    held.fixturePath = *path;
    held.baseSequence = window.sequence()->id();
    held.ready = true;
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
