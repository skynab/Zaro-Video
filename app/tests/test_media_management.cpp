// Media: what comes in, what it points at, and where it lives.
//
// Driven through the real window against the real compositor. See GuiFixture.h
// for what is shared and why.

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <system_error>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/ColorPipeline.h"

#include "../FrameGrab.h"
#include "GuiFixture.h"

// The suite was written inside main(), which had this at file scope; the
// bodies still say `model::` and `Status` unqualified.
using namespace zaro;

using zaro::app::dragOnTimeline;
using zaro::app::settledGrab;

// meanGray is named here rather than aliased inside each test, which is how it
// arrived: the suite was one main() sharing local lambdas, and the conversion
// left every test opening with a reference bound to this function. MSVC's
// constexpr evaluator crashes on a call made through such a reference when the
// result initialises a const double -- an internal compiler error, not a
// diagnostic -- so `const double bright = meanGray(image);` took the whole
// build down. Calling the function by its own name is what every other
// compiler was doing anyway.
using zaro::app::testing::meanGray;

// Interpreting footage: telling the program what a file's curve really
// is, and having the decoder believe it.
//
// Checked at the frame rather than at the picture, because this fixture
// is pure black and pure white with nothing in between: black stays
// black under any curve and white clips under all of them, so the
// preview cannot tell two curves apart on it. That the curves
// themselves are right, and that the GPU and the CPU agree about them,
// is what the golden-frame tests are for. What is left to check here is
// that the override reaches the decoder at all -- and it can fail,
// because without the plumbing the frame comes back tagged as the file
// described it.
TEST_CASE("Interpreting a file's colour curve", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;

    const auto interpretMediaId = window.project().media().front().id;
    const auto interpretRate =
        window.project().findSequence(window.project().activeSequence())->frameRate();
    const auto probeAt = zaro::time::RationalTime{4, interpretRate};

    auto tagged = window.frames()->sourceFrameFor(interpretMediaId, probeAt);
    if (!tagged) {
        zaro::app::testing::failf("%s\n", tagged.error().toString().c_str());
    }
    const auto taggedTransfer = (*tagged)->color().transfer;

    for (zaro::model::MediaRef& media : window.project().mediaMutable()) {
        if (media.id == interpretMediaId) {
            media.transferOverride = zaro::media::TransferFunction::SLog3;
        }
    }
    window.renderCache().clear();
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }

    auto overridden = window.frames()->sourceFrameFor(interpretMediaId, probeAt);
    if (!overridden) {
        zaro::app::testing::failf("%s\n", overridden.error().toString().c_str());
    }
    const auto overriddenTransfer = (*overridden)->color().transfer;
    std::printf("  interpret footage: decoded as %s, then as %s\n",
                zaro::media::toString(taggedTransfer), zaro::media::toString(overriddenTransfer));
    if (overriddenTransfer != zaro::media::TransferFunction::SLog3 ||
        taggedTransfer == overriddenTransfer) {
        zaro::app::testing::failf("the curve override did not reach the decoder\n");
    }
    // And the preview still renders through it rather than refusing.
    window.setPosition(probeAt);
    window.monitor()->update();
    QApplication::processEvents();
    if (!window.monitor()->lastError().isEmpty()) {
        zaro::app::testing::failf("rendering log footage reported %s\n",
                                  window.monitor()->lastError().toUtf8().constData());
    }

    // The gamut, the same way. A container that carries no primaries tag is
    // read as BT.709, so phone footage that is really Display P3 composites as
    // though its red were Rec.709's -- and, since Phase 8c, saying so actually
    // changes the picture rather than being a label nothing reads.
    for (zaro::model::MediaRef& media : window.project().mediaMutable()) {
        if (media.id == interpretMediaId) {
            media.transferOverride = zaro::media::TransferFunction::Unknown;
            media.primariesOverride = zaro::media::ColorPrimaries::BT2020;
        }
    }
    window.renderCache().clear();
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    auto regamuted = window.frames()->sourceFrameFor(interpretMediaId, probeAt);
    if (!regamuted) {
        zaro::app::testing::failf("%s\n", regamuted.error().toString().c_str());
    }
    if ((*regamuted)->color().primaries != zaro::media::ColorPrimaries::BT2020) {
        zaro::app::testing::failf("the gamut override did not reach the decoder (%s)\n",
                                  zaro::media::toString((*regamuted)->color().primaries));
    }
    // That the tag then changes the picture is checked in core, against a
    // coloured source -- "A wide-gamut source is brought into the working
    // space" in test_color_pipeline.cpp. It cannot be checked here: the
    // fixture clip is black frames and white flashes, and a conversion between
    // two D65 gamuts leaves neutrals exactly alone by construction. Asserting
    // a colour change on this footage would be testing the fixture rather than
    // the code, and it would pass or fail on which clip somebody generated.
    zaro::render::RgbaImage wideLinear;
    if (Status ok = zaro::render::toLinear(**regamuted, wideLinear); !ok) {
        zaro::app::testing::failf("reading the footage as BT.2020 stopped it decoding: %s\n",
                                  ok.error().toString().c_str());
    }
    std::printf("  interpret gamut: decoded as %s\n",
                zaro::media::toString((*regamuted)->color().primaries));

    for (zaro::model::MediaRef& media : window.project().mediaMutable()) {
        media.transferOverride = zaro::media::TransferFunction::Unknown;
        media.primariesOverride = zaro::media::ColorPrimaries::Unknown;
    }
    window.renderCache().clear();
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    auto restored = window.frames()->sourceFrameFor(interpretMediaId, probeAt);
    if (!restored) {
        zaro::app::testing::failf("%s\n", restored.error().toString().c_str());
    }
    // Clearing an override puts the file back to what its container says, which
    // is what makes it a correction rather than a one-way door.
    if ((*restored)->color().primaries == zaro::media::ColorPrimaries::BT2020 &&
        (*regamuted)->color().primaries != (*restored)->color().primaries) {
        zaro::app::testing::failf("clearing the gamut override did not take it back\n");
    }

    window.monitor()->update();
    QApplication::processEvents();
}

// Getting back to the source: a subclip made from what is marked, match
// frame from a clip on the timeline, and replacing a clip's footage.
//
// Self-contained, and late: it moves the playhead a long way and the
// timeline scrolls to follow, and the blocks that aim real mouse events
// at fixed coordinates run before this.
TEST_CASE("A subclip made from what is marked", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;

    const auto sourceSequenceId = window.project().activeSequence();
    const auto sourceTrackId =
        window.project().findSequence(sourceSequenceId)->videoTracks().front().id();
    const auto sourceRate = window.project().findSequence(sourceSequenceId)->frameRate();
    const zaro::model::MediaRef& sourceMedia = window.project().media().front();

    // A subclip of a marked range, through the monitor's own button.
    window.sourceMonitor()->load(sourceMedia);
    window.sourceMonitor()->step(24);
    window.sourceMonitor()->markIn();
    window.sourceMonitor()->step(48);
    window.sourceMonitor()->markOut();
    QApplication::processEvents();

    auto* subclipButton = window.sourceMonitor()->findChild<QPushButton*>("make-subclip");
    if (subclipButton == nullptr) {
        zaro::app::testing::failf("the source monitor has no subclip button\n");
    }
    const auto markedRange = window.sourceMonitor()->markedRange();
    const std::size_t subclipsBefore = window.project().subclips().size();
    subclipButton->click();
    QApplication::processEvents();

    if (window.project().subclips().size() != subclipsBefore + 1) {
        zaro::app::testing::failf("the subclip button made no subclip\n");
    }
    const zaro::model::Subclip made = window.project().subclips().back();
    if (!markedRange || made.range.duration() != markedRange->duration()) {
        zaro::app::testing::failf("the subclip does not hold what was marked\n");
    }
    std::printf("  subclip: %lld frames of %s\n",
                static_cast<long long>(made.range.duration().frames()), made.name.c_str());

    // Match frame, from a clip this block places itself.
    zaro::model::Clip probe;
    probe.id = window.project().ids().next<zaro::model::ClipTag>();
    probe.source = sourceMedia.id;
    probe.name = "match probe";
    probe.sourceRange = zaro::time::TimeRange{zaro::time::RationalTime{60, sourceRate},
                                              zaro::time::RationalTime{40, sourceRate}};
    probe.timelineRange = zaro::time::TimeRange{zaro::time::RationalTime{0, sourceRate},
                                                zaro::time::RationalTime{40, sourceRate}};
    const auto probeId = probe.id;
    auto placedProbe =
        zaro::edit::makeOverwrite(window.project(), {sourceSequenceId, sourceTrackId}, probe);
    if (!placedProbe) {
        zaro::app::testing::failf("%s\n", placedProbe.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*placedProbe));

    const auto matchAt = zaro::time::RationalTime{10, sourceRate};
    const zaro::model::Clip* matchClip =
        window.project().findSequence(sourceSequenceId)->findTrack(sourceTrackId)->find(probeId);
    const auto expected = matchClip->activeSourceTimeAt(matchAt);
    timeline->selectOnly(sourceTrackId, probeId);
    window.setPosition(matchAt);
    QApplication::processEvents();
    // Somewhere else first, so landing on the right frame cannot be
    // where it already was.
    window.sourceMonitor()->step(-2000);
    QApplication::processEvents();

    QKeyEvent matchKey(QEvent::KeyPress, Qt::Key_F, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &matchKey);
    QApplication::processEvents();

    const auto landed = window.sourceMonitor()->position();
    std::printf("  match frame: source frame %lld, clip says %lld\n",
                static_cast<long long>(landed.frames()), static_cast<long long>(expected.frames()));
    if (landed.rescaledTo(expected.rate()).frames() != expected.frames()) {
        zaro::app::testing::failf("match frame landed on the wrong frame\n");
    }

    // Replace footage: a second reference to the same file stands in
    // for a graded version, and the cut must not move.
    zaro::model::MediaRef graded = sourceMedia;
    graded.id = window.project().ids().next<zaro::model::MediaRefTag>();
    graded.name = "graded";
    const auto gradedId = window.project().addMedia(graded);

    const auto beforeStart = matchClip->start();
    const auto beforeDuration = matchClip->timelineRange.duration();
    const auto beforeIn = matchClip->sourceRange.start();
    window.replaceSelectedSource(gradedId);
    QApplication::processEvents();

    const zaro::model::Clip* replaced =
        window.project().findSequence(sourceSequenceId)->findTrack(sourceTrackId)->find(probeId);
    if (replaced == nullptr || replaced->source != gradedId) {
        zaro::app::testing::failf("replacing the footage did not reach the clip\n");
    }
    if (replaced->start() != beforeStart || replaced->timelineRange.duration() != beforeDuration ||
        replaced->sourceRange.start() != beforeIn) {
        zaro::app::testing::failf("replacing the footage moved the cut\n");
    }
    std::printf("  replace footage: the cut stayed at %lld for %lld frames\n",
                static_cast<long long>(beforeStart.frames()),
                static_cast<long long>(beforeDuration.frames()));

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    // Neither adding media nor adding a subclip is a command, so undo
    // does not reach them; this block puts the project back itself.
    window.project().mediaMutable().pop_back();
    window.project().removeSubclip(made.id);
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    window.monitor()->update();
    QApplication::processEvents();
}

// Proxies: the swap, and the promise that export ignores it.
//
// The proxy is made here rather than pointed at. It used to be a file
// in a scratch folder from the session that wrote this test, which
// meant the check passed on that machine that week and silently did
// nothing ever after -- it was found dead, months later, by a run on a
// clean temp directory.
TEST_CASE("A proxy is swapped in for preview and ignored on export", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;

    const std::filesystem::path proxyFolder =
        std::filesystem::temp_directory_path() / "zaro-selftest-proxy-swap";
    zaro::app::testing::discard(proxyFolder);
    std::filesystem::create_directories(proxyFolder);
    for (auto& media : window.project().mediaMutable()) {
        zaro::platform::ffmpeg::ProxySettings settings;
        settings.source = media.path;
        settings.destination =
            (proxyFolder / (std::filesystem::path{media.path}.stem().string() + "-proxy.mov"))
                .string();
        settings.width = 96;  // much smaller, so which file was read is unmistakable
        auto made = zaro::platform::ffmpeg::makeProxy(settings);
        if (!made) {
            zaro::app::testing::failf("%s\n", made.error().toString().c_str());
        }
        media.proxyPath = made->path;
    }

    std::int64_t brightFrame = 0;
    double brightness = 0.0;
    for (std::int64_t frame = 0; frame < 40; ++frame) {
        window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
        const double gray = meanGray(settledGrab(window.monitor()));
        if (gray > brightness) {
            brightness = gray;
            brightFrame = frame;
        }
    }
    window.setPosition(zaro::time::RationalTime{brightFrame, sequence.frameRate()});
    QApplication::processEvents();
    const double fromOriginal = meanGray(settledGrab(window.monitor()));

    window.project().setUsingProxies(true);
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    window.monitor()->update();
    QApplication::processEvents();
    const double viaProxy = meanGray(settledGrab(window.monitor()));

    // What was read, rather than what it looked like: the proxy is a
    // faithful copy at a quarter of the width, so judging it by
    // brightness would be judging the resampler. The decoded frame's
    // size says which file the pixels came from and nothing else does.
    const auto probeAt = zaro::time::RationalTime{brightFrame, sequence.frameRate()};
    const auto mediaId = window.project().media().front().id;
    auto small = window.frameSource().imageFor(mediaId, probeAt);
    if (!small) {
        zaro::app::testing::failf("the proxy does not decode\n");
    }
    const std::int32_t proxyWidth = (*small)->width();
    std::printf("  proxies: %.1f from the original, %.1f from a %d-wide proxy\n", fromOriginal,
                viaProxy, proxyWidth);
    if (proxyWidth != 96) {
        zaro::app::testing::failf("reading proxies gave a %d-wide picture\n", proxyWidth);
    }

    // Back to the originals, which is also what export uses whatever
    // this toggle says.
    window.project().setUsingProxies(false);
    // The proxy paths are cleared before the reopen rather than after it:
    // reopening while the project still names them opens the proxy files
    // again, and a file the media source has open is one that cannot be
    // deleted underneath it.
    for (auto& media : window.project().mediaMutable()) {
        media.proxyPath.clear();
    }
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    zaro::app::testing::discard(proxyFolder);
    window.monitor()->update();
    QApplication::processEvents();
}

// Metadata and search, through the real bin.
TEST_CASE("Metadata and search, through the real bin", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;

    auto* binPanel = window.bin();
    auto* binSearch = binPanel->findChild<QLineEdit*>();
    auto* binList = binPanel->findChild<QListWidget*>();
    if (binSearch == nullptr || binList == nullptr) {
        zaro::app::testing::failf("the bin has no search box or list\n");
    }
    // Media only. The bin grew folders, and a folder is a row in the same list
    // -- counting those made "everything" one too many and made a search that
    // matched one file look like it matched two. A header is enabled so it can
    // be clicked shut but never selectable, which is what tells them apart.
    const auto visibleRows = [&] {
        int shown = 0;
        for (int index = 0; index < binList->count(); ++index) {
            const QListWidgetItem* item = binList->item(index);
            if (item->isHidden() || (item->flags() & Qt::ItemIsSelectable) == 0) {
                continue;
            }
            ++shown;
        }
        return shown;
    };

    // A second file, so "found it" and "found everything" are
    // different answers: with one item in the bin every search that
    // matches at all looks like a search that works.
    auto probedSecond =
        zaro::platform::ffmpeg::probe(zaro::app::testing::mediaFixture("ladder_prores.mov"));
    if (!probedSecond) {
        zaro::app::testing::failf("%s (run testdata/generate.sh)\n",
                                  probedSecond.error().toString().c_str());
    }
    zaro::model::MediaRef second;
    second.path = zaro::app::testing::mediaFixture("ladder_prores.mov");
    second.name = "ladder";
    second.info = *probedSecond;
    auto broughtIn = zaro::edit::makeImportMedia(window.project(), second);
    if (!broughtIn) {
        zaro::app::testing::failf("%s\n", broughtIn.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*broughtIn));
    window.bin()->refresh();
    QApplication::processEvents();

    binSearch->setText("");
    QApplication::processEvents();
    const int everything = visibleRows();
    if (everything < 2) {
        zaro::app::testing::failf("the bin should hold two files, not %d\n", everything);
    }

    // Searchable by a technical fact that is nowhere in the file's
    // name: what the bin knows should be what the bin can find. The
    // ladder is the ProRes one, and nothing else in the bin is.
    const auto* first = &window.project().media().back();
    const auto* firstVideo = first->info.primaryVideo();
    if (firstVideo == nullptr || firstVideo->codecName.empty()) {
        zaro::app::testing::failf("the fixture has no codec to search for\n");
    }
    binSearch->setText(QString::fromStdString(firstVideo->codecName));
    QApplication::processEvents();
    const int byCodec = visibleRows();

    binSearch->setText("definitelynotacodec");
    QApplication::processEvents();
    const int byNonsense = visibleRows();

    // A note, written the way the Notes button writes it, and then
    // found by searching for a word only the note contains.
    binPanel->setNotes(first->id, "boom in shot, take 3");
    QApplication::processEvents();
    binSearch->setText("boom");
    QApplication::processEvents();
    const int byNote = visibleRows();

    binSearch->setText(QString("boom %1").arg(QString::fromStdString(firstVideo->codecName)));
    QApplication::processEvents();
    const int byBoth = visibleRows();

    binSearch->setText("boom h264xyz");
    QApplication::processEvents();
    const int byBothWrong = visibleRows();

    std::printf(
        "  bin search: %d items, %d by codec, %d by note, %d by both, %d by "
        "nonsense\n",
        everything, byCodec, byNote, byBoth, byNonsense);
    // The two fixtures are in different codecs, so a codec search has
    // to pick out one of them rather than matching both.
    if (byCodec != 1) {
        zaro::app::testing::failf("searching by codec found %d of %d\n", byCodec, everything);
    }
    if (byNonsense != 0) {
        zaro::app::testing::failf("a nonsense search still matched\n");
    }
    if (byNote != 1 || byBoth != 1) {
        zaro::app::testing::failf("the note is not searchable (%d, %d)\n", byNote, byBoth);
    }
    if (byBothWrong != 0) {
        zaro::app::testing::failf("every word has to match, and one of these does not\n");
    }
    if (window.project().findMedia(first->id)->notes.empty()) {
        zaro::app::testing::failf("the note did not reach the project\n");
    }
    // And it undoes, like every other edit.
    window.commands().undo(window.project());
    if (!window.project().findMedia(first->id)->notes.empty()) {
        zaro::app::testing::failf("undoing did not take the note back\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    binSearch->setText("");
    QApplication::processEvents();
    window.bin()->refresh();
    QApplication::processEvents();
}

// Transcoding on the way in, through the real bin.
TEST_CASE("Transcoding on the way in", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;

    const std::filesystem::path ingestRoot =
        std::filesystem::temp_directory_path() / "zaro-selftest-ingest";
    zaro::app::testing::discard(ingestRoot);
    std::filesystem::create_directories(ingestRoot);

    const std::size_t mediaBefore = window.project().media().size();
    if (Status done =
            window.bin()->importTranscoded({zaro::app::testing::mediaFixture("shaky_texture.mov")},
                                           ingestRoot.string(), "prores_ks");
        !done) {
        zaro::app::testing::failf("%s\n", done.error().toString().c_str());
    }
    if (window.project().media().size() != mediaBefore + 1) {
        zaro::app::testing::failf("the transcoded file was not imported\n");
    }
    const auto& ingested = window.project().media().back();
    const auto* ingestedVideo = ingested.info.primaryVideo();
    if (ingestedVideo == nullptr || ingestedVideo->codecName != "prores") {
        zaro::app::testing::failf("the imported file is not in the ingest codec\n");
    }
    if (std::filesystem::path{ingested.path}.parent_path() != ingestRoot) {
        zaro::app::testing::failf("the project points somewhere unexpected\n");
    }
    // The original is untouched: ingesting must not eat rushes.
    if (!std::filesystem::exists(zaro::app::testing::mediaFixture("shaky_texture.mov"))) {
        zaro::app::testing::failf("ingesting moved the original\n");
    }
    // And it is the same picture, frame for frame.
    auto sourceInfo =
        zaro::platform::ffmpeg::probe(zaro::app::testing::mediaFixture("shaky_texture.mov"));
    if (!sourceInfo || sourceInfo->primaryVideo() == nullptr) {
        zaro::app::testing::failf("the fixture does not probe\n");
    }
    if (ingestedVideo->width != sourceInfo->primaryVideo()->width ||
        ingestedVideo->durationInFrames().frames() !=
            sourceInfo->primaryVideo()->durationInFrames().frames()) {
        zaro::app::testing::failf("the transcode changed the size or the length\n");
    }
    // Where it came from is recorded, since the project now points at
    // the copy.
    if (ingested.notes.find("shaky_texture.mov") == std::string::npos) {
        zaro::app::testing::failf("nothing says where the ingested file came from\n");
    }
    std::printf("  ingest: %s at %dx%d, %lld frames\n", ingestedVideo->codecName.c_str(),
                ingestedVideo->width, ingestedVideo->height,
                static_cast<long long>(ingestedVideo->durationInFrames().frames()));

    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    if (!window.frameSource().imageFor(ingested.id,
                                       zaro::time::RationalTime{2, zaro::time::rates::fps25})) {
        zaro::app::testing::failf("the ingested file does not decode\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    // Deleted after the undo and the reopen, never before: while the project
    // still names these files, the media source holds a decoder on each one.
    zaro::app::testing::discard(ingestRoot);
    window.renderCache().clear();
    window.monitor()->update();
    QApplication::processEvents();
}

// Making a proxy, then editing against it.
TEST_CASE("Making a proxy, then editing against it", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;

    const std::filesystem::path proxyRoot =
        std::filesystem::temp_directory_path() / "zaro-selftest-proxy";
    zaro::app::testing::discard(proxyRoot);
    std::filesystem::create_directories(proxyRoot);
    const std::filesystem::path heavyPath = proxyRoot / "proxied_clip.mov";
    std::filesystem::copy_file(zaro::app::testing::mediaFixture("shaky_texture.mov"), heavyPath);

    auto probed = zaro::platform::ffmpeg::probe(heavyPath.string());
    if (!probed) {
        zaro::app::testing::failf("%s (run testdata/generate.sh)\n",
                                  probed.error().toString().c_str());
    }
    zaro::model::MediaRef heavy;
    heavy.path = heavyPath.string();
    heavy.name = "proxied_clip";
    heavy.info = *probed;
    auto imported = zaro::edit::makeImportMedia(window.project(), heavy);
    if (!imported) {
        zaro::app::testing::failf("%s\n", imported.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*imported));
    const auto heavyId = window.project().media().back().id;
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }

    auto made = window.buildProxy(heavyId, 160);
    if (!made) {
        zaro::app::testing::failf("%s\n", made.error().toString().c_str());
    }
    std::printf(
        "  proxy: %dx%d, %lld frames, %.0f%% of the size\n", made->width, made->height,
        static_cast<long long>(made->frames),
        100.0 * static_cast<double>(made->proxyBytes) / static_cast<double>(made->sourceBytes));

    const auto* proxied = window.project().findMedia(heavyId);
    if (proxied == nullptr || proxied->proxyPath != made->path) {
        zaro::app::testing::failf("the proxy was not attached\n");
    }
    // Smaller, or it is not a proxy. The first version defaulted to
    // the container's codec and produced a file eight times the size
    // of the original.
    if (!(made->proxyBytes < made->sourceBytes / 2)) {
        zaro::app::testing::failf("the proxy is not smaller than the media\n");
    }
    // The one thing a proxy must not do: change how long the media is.
    const auto proxyInfo = zaro::platform::ffmpeg::probe(made->path);
    if (!proxyInfo || proxyInfo->primaryVideo() == nullptr) {
        zaro::app::testing::failf("the proxy does not probe\n");
    }
    if (proxyInfo->primaryVideo()->durationInFrames().frames() !=
            probed->primaryVideo()->durationInFrames().frames() ||
        proxyInfo->primaryVideo()->frameRate != probed->primaryVideo()->frameRate) {
        zaro::app::testing::failf("the proxy is a different length from the media\n");
    }

    // And switching to proxies really reads the smaller file.
    const auto probeAt = zaro::time::RationalTime{3, zaro::time::rates::fps25};
    window.project().setUsingProxies(true);
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    auto onProxy = window.frameSource().imageFor(heavyId, probeAt);
    if (!onProxy) {
        zaro::app::testing::failf("the proxy does not decode\n");
    }
    const std::int32_t proxyWidth = (*onProxy)->width();
    window.project().setUsingProxies(false);
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    auto onOriginal = window.frameSource().imageFor(heavyId, probeAt);
    if (!onOriginal) {
        zaro::app::testing::failf("the original does not decode\n");
    }
    if (!(proxyWidth < (*onOriginal)->width())) {
        zaro::app::testing::failf("reading proxies gave the same size picture (%d vs %d)\n",
                                  proxyWidth, (*onOriginal)->width());
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    // Deleted after the undo and the reopen, never before: while the project
    // still names these files, the media source holds a decoder on each one.
    zaro::app::testing::discard(proxyRoot);
    window.renderCache().clear();
    window.monitor()->update();
    QApplication::processEvents();
}

// Consolidating: gather the project's media into one folder.
TEST_CASE("Consolidating the project's media into one folder", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;

    const std::filesystem::path gatherRoot =
        std::filesystem::temp_directory_path() / "zaro-selftest-consolidate";
    zaro::app::testing::discard(gatherRoot);
    std::filesystem::create_directories(gatherRoot / "cards");
    const std::filesystem::path scattered = gatherRoot / "cards" / "gathered_clip.mov";
    std::filesystem::copy_file(zaro::app::testing::mediaFixture("shaky_texture.mov"), scattered);

    auto probed = zaro::platform::ffmpeg::probe(scattered.string());
    if (!probed) {
        zaro::app::testing::failf("%s (run testdata/generate.sh)\n",
                                  probed.error().toString().c_str());
    }
    zaro::model::MediaRef loose;
    loose.path = scattered.string();
    loose.name = "gathered_clip";
    loose.info = *probed;
    auto imported = zaro::edit::makeImportMedia(window.project(), loose);
    if (!imported) {
        zaro::app::testing::failf("%s\n", imported.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*imported));
    const auto looseId = window.project().media().back().id;
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }

    const std::filesystem::path into = gatherRoot / "project media";
    auto gathered = window.consolidateMedia(into.string());
    if (!gathered) {
        zaro::app::testing::failf("%s\n", gathered.error().toString().c_str());
    }
    std::printf("  consolidate: %zu files gathered, %.1f MB copied, %zu missing\n",
                gathered->files.size(), static_cast<double>(gathered->bytes) / (1024.0 * 1024.0),
                gathered->missing.size());

    const auto* moved = window.project().findMedia(looseId);
    if (moved == nullptr || std::filesystem::path{moved->path}.parent_path() != into) {
        zaro::app::testing::failf("the project does not point into the folder\n");
    }
    if (!std::filesystem::exists(scattered)) {
        zaro::app::testing::failf(
            "consolidating moved the original instead of "
            "copying it\n");
    }
    // And the copy is the thing that plays now.
    const auto probeAt = zaro::time::RationalTime{2, zaro::time::rates::fps25};
    if (!window.frameSource().imageFor(looseId, probeAt)) {
        zaro::app::testing::failf("the consolidated media does not decode\n");
    }
    // The fixture media the project came with is in there too: what is
    // gathered is the project, not the selection.
    if (gathered->files.size() < 2) {
        zaro::app::testing::failf("only the new file was gathered (%zu)\n", gathered->files.size());
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    // Deleted after the undo and the reopen, never before: while the project
    // still names these files, the media source holds a decoder on each one.
    zaro::app::testing::discard(gatherRoot);
    window.renderCache().clear();
    window.monitor()->update();
    QApplication::processEvents();
}

// Relinking, with a file that really moves.
TEST_CASE("Relinking a file that really moved", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;

    const std::filesystem::path relinkRoot =
        std::filesystem::temp_directory_path() / "zaro-selftest-relink";
    zaro::app::testing::discard(relinkRoot);
    std::filesystem::create_directories(relinkRoot / "before");
    std::filesystem::create_directories(relinkRoot / "after" / "deeper");
    const std::filesystem::path was = relinkRoot / "before" / "moved_clip.mov";
    std::filesystem::copy_file(zaro::app::testing::mediaFixture("shaky_texture.mov"), was);

    auto probed = zaro::platform::ffmpeg::probe(was.string());
    if (!probed) {
        zaro::app::testing::failf("%s (run testdata/generate.sh)\n",
                                  probed.error().toString().c_str());
    }
    zaro::model::MediaRef moving;
    moving.path = was.string();
    moving.name = "moved_clip";
    moving.info = *probed;
    if (auto digest = zaro::media::contentDigest(was.string())) {
        moving.contentDigest = *digest;
    }
    auto imported = zaro::edit::makeImportMedia(window.project(), moving);
    if (!imported) {
        zaro::app::testing::failf("%s\n", imported.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*imported));
    const auto movingId = window.project().media().back().id;
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    // It decodes where it is.
    const auto probeAt = zaro::time::RationalTime{2, zaro::time::rates::fps25};
    if (!window.frameSource().imageFor(movingId, probeAt)) {
        zaro::app::testing::failf("the copied fixture does not decode\n");
    }

    // Now move it, the way a card being copied to a server does.
    //
    // The project stops naming it first. Windows will not rename a file this
    // process has open, and the media source opens every file the project
    // names -- so the decoder is dropped, the file moves, and the old path goes
    // straight back, which is precisely the state relinking is about: a project
    // pointing at a file that is not where it says it is.
    const std::filesystem::path now = relinkRoot / "after" / "deeper" / "moved_clip.mov";
    const std::string wasPath = was.string();
    for (auto& media : window.project().mediaMutable()) {
        if (media.id == movingId) {
            media.path.clear();
        }
    }
    static_cast<void>(window.reopenMedia());
    zaro::app::testing::moveAside(was, now);
    for (auto& media : window.project().mediaMutable()) {
        if (media.id == movingId) {
            media.path = wasPath;
        }
    }
    // Reopening tolerates a missing file -- one unreadable clip must
    // not stop a project opening -- so what says it is gone is that it
    // no longer decodes.
    static_cast<void>(window.reopenMedia());
    if (window.frameSource().imageFor(movingId, probeAt)) {
        zaro::app::testing::failf("a file that moved still decodes\n");
    }

    auto report = window.relinkMedia(relinkRoot.string());
    if (!report) {
        zaro::app::testing::failf("%s\n", report.error().toString().c_str());
    }
    std::printf("  relink: %zu found, %zu still missing, %d files looked at\n",
                report->matches.size(), report->stillMissing.size(), report->examined);
    if (report->matches.size() != 1 || !report->matches.front().byContent) {
        zaro::app::testing::failf("the moved file was not matched by its content\n");
    }
    const auto* relinked = window.project().findMedia(movingId);
    if (relinked == nullptr || relinked->path != now.string()) {
        zaro::app::testing::failf("the project still points at the old path\n");
    }
    // And it decodes again, which is the whole point.
    if (!window.frameSource().imageFor(movingId, probeAt)) {
        zaro::app::testing::failf("the relinked media does not decode\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    // Deleted after the undo and the reopen, never before: while the project
    // still names these files, the media source holds a decoder on each one.
    zaro::app::testing::discard(relinkRoot);
    window.renderCache().clear();
    window.monitor()->update();
    QApplication::processEvents();
}

// The media browser: look through a folder, take what is wanted.
TEST_CASE("The media browser imports what is wanted", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    // Looked up per test rather than held by the fixture: a test that adds a
    // track or a sequence reallocates the vectors these point into.
    [[maybe_unused]] auto* timeline = window.timeline();
    [[maybe_unused]] const auto& sequence = *window.sequence();
    [[maybe_unused]] const auto& videoTrack = sequence.videoTracks().front();
    [[maybe_unused]] const auto original = videoTrack.clips().front();
    [[maybe_unused]] const auto row = timeline->rowFor(videoTrack.id());
    REQUIRE(row.has_value());
    [[maybe_unused]] const int y = row->top + row->height / 2;

    const std::filesystem::path browseRoot =
        std::filesystem::temp_directory_path() / "zaro-selftest-browse";
    zaro::app::testing::discard(browseRoot);
    std::filesystem::create_directories(browseRoot / "DAY 2");
    std::filesystem::copy_file(zaro::app::testing::mediaFixture("shaky_texture.mov"),
                               browseRoot / "DAY 2" / "A001.mov");
    std::filesystem::copy_file(zaro::app::testing::mediaFixture("wide_texture.mp4"),
                               browseRoot / "DAY 2" / "A002.mp4");
    // The things a card also holds, which are not footage.
    {
        std::ofstream junk{browseRoot / "DAY 2" / "MEDIAPRO.XML"};
        junk << "<manifest/>";
        std::ofstream hidden{browseRoot / "DAY 2" / ".DS_Store"};
        hidden << "x";
    }

    auto* browser = window.browseMedia();
    if (browser == nullptr) {
        zaro::app::testing::failf("there is no media browser\n");
    }
    if (Status shown = browser->showFolder(browseRoot.string()); !shown) {
        zaro::app::testing::failf("%s\n", shown.error().toString().c_str());
    }
    auto* browserList = browser->findChild<QListWidget*>("browser-list");
    if (browserList == nullptr) {
        zaro::app::testing::failf("the browser has no list\n");
    }
    if (browserList->count() != 1) {
        zaro::app::testing::failf("the top folder should hold one folder, not %d\n",
                                  browserList->count());
    }

    // Into the folder, where the footage is and the junk is not.
    if (Status shown = browser->showFolder((browseRoot / "DAY 2").string()); !shown) {
        zaro::app::testing::failf("%s\n", shown.error().toString().c_str());
    }
    if (browserList->count() != 2) {
        zaro::app::testing::failf(
            "the card should list two files, not %d -- the manifest "
            "and the dot file are not footage\n",
            browserList->count());
    }

    const std::size_t mediaBeforeBrowse = window.project().media().size();
    browser->selectAllFiles();
    auto imported = browser->importSelected();
    if (!imported) {
        zaro::app::testing::failf("%s\n", imported.error().toString().c_str());
    }
    if (*imported != 2 || window.project().media().size() != mediaBeforeBrowse + 2) {
        zaro::app::testing::failf("%d files imported, not two\n", *imported);
    }
    // Importing the same folder again adds nothing: two entries for one
    // file would be two things to grade and relink.
    auto again = browser->importSelected();
    if (!again || *again != 0) {
        zaro::app::testing::failf("importing twice made a second entry\n");
    }
    std::printf("  media browser: %d imported, %zu now in the project\n", *imported,
                window.project().media().size());

    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    const auto justImported = window.project().media().back().id;
    if (!window.frameSource().imageFor(justImported,
                                       zaro::time::RationalTime{1, zaro::time::rates::fps25})) {
        zaro::app::testing::failf("what the browser imported does not decode\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    // Deleted after the undo and the reopen, never before: while the project
    // still names these files, the media source holds a decoder on each one.
    zaro::app::testing::discard(browseRoot);
    QApplication::processEvents();
}

// Files let go of over the media pane.
//
// The drop is delivered as Qt delivers it -- a drag-enter to ask whether the
// pane will have them, then the drop itself -- because what this covers is the
// wiring: that the pane says yes to footage, no to a manifest, and that saying
// yes ends in the same import the file dialog performs.
TEST_CASE("Files dropped on the media pane are imported", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;

    const std::filesystem::path dropRoot =
        std::filesystem::temp_directory_path() / "zaro-selftest-drop";
    zaro::app::testing::discard(dropRoot);
    std::filesystem::create_directories(dropRoot);
    std::filesystem::copy_file(zaro::app::testing::mediaFixture("shaky_texture.mov"),
                               dropRoot / "B001.mov");
    std::filesystem::copy_file(zaro::app::testing::mediaFixture("tone_48k.wav"),
                               dropRoot / "B001.wav");
    {
        std::ofstream junk{dropRoot / "MEDIAPRO.XML"};
        junk << "<manifest/>";
    }

    auto* bin = window.bin();
    if (bin == nullptr) {
        zaro::app::testing::failf("there is no media pane\n");
    }

    // The events below go straight to the pane, which is not how a drag from
    // the file manager arrives: Qt hands it to the first widget under the
    // pointer that takes drops, walking up from whichever child is there. A
    // child that took drops -- a search field, a list in drag-drop mode --
    // would swallow the files before the pane ever saw them, so no child may.
    for (QWidget* under = bin->childAt(bin->width() / 2, bin->height() / 2);
         under != nullptr && under != bin; under = under->parentWidget()) {
        if (under->acceptDrops()) {
            zaro::app::testing::failf("%s sits over the pane and takes drops itself\n",
                                      under->metaObject()->className());
        }
    }

    const auto drag = [bin](const QList<QUrl>& urls) {
        QMimeData mime;
        mime.setUrls(urls);
        QDragEnterEvent entering{QPoint{bin->width() / 2, bin->height() / 2}, Qt::CopyAction, &mime,
                                 Qt::LeftButton, Qt::NoModifier};
        QApplication::sendEvent(bin, &entering);
        return entering.isAccepted();
    };
    // The enter comes first, as it does in a real drag: Qt only delivers a
    // drop to a widget that has just said it would take one.
    const auto drop = [bin, drag](const QList<QUrl>& urls) {
        static_cast<void>(drag(urls));
        QMimeData mime;
        mime.setUrls(urls);
        QDropEvent dropping{QPointF{bin->width() / 2.0, bin->height() / 2.0}, Qt::CopyAction, &mime,
                            Qt::LeftButton, Qt::NoModifier};
        QApplication::sendEvent(bin, &dropping);
        QApplication::processEvents();
    };

    // A manifest is not footage, and the pane should not offer to take it.
    if (drag({QUrl::fromLocalFile(QString::fromStdString((dropRoot / "MEDIAPRO.XML").string()))})) {
        zaro::app::testing::failf("the pane offered to import a manifest\n");
    }

    const QList<QUrl> footage{
        QUrl::fromLocalFile(QString::fromStdString((dropRoot / "B001.mov").string())),
        QUrl::fromLocalFile(QString::fromStdString((dropRoot / "B001.wav").string()))};
    if (!drag(footage)) {
        zaro::app::testing::failf("the pane refused a picture and a sound file\n");
    }

    const std::size_t before = window.project().media().size();
    drop(footage);
    if (window.project().media().size() != before + 2) {
        zaro::app::testing::failf("%zu media after the drop, not %zu\n",
                                  window.project().media().size(), before + 2);
    }

    // The same files again, and the folder that holds them: neither adds a
    // second entry pointing at a file the project already has.
    drop(footage);
    drop({QUrl::fromLocalFile(QString::fromStdString(dropRoot.string()))});
    if (window.project().media().size() != before + 2) {
        zaro::app::testing::failf("dropping the same files twice made %zu entries, not %zu\n",
                                  window.project().media().size(), before + 2);
    }

    // A folder of footage the project has never seen is listed and taken.
    const std::filesystem::path second = dropRoot / "DAY 3";
    std::filesystem::create_directories(second);
    std::filesystem::copy_file(zaro::app::testing::mediaFixture("wide_texture.mp4"),
                               second / "C001.mp4");
    drop({QUrl::fromLocalFile(QString::fromStdString(second.string()))});
    if (window.project().media().size() != before + 3) {
        zaro::app::testing::failf("the dropped folder added %zu, not one\n",
                                  window.project().media().size() - before - 2);
    }

    std::printf("  dropped media: %zu now in the project\n", window.project().media().size());

    // Let go of the files before deleting them, and do not insist that the
    // delete succeeds. Everything the pane lists is open somewhere -- a decoder
    // for the monitor, a poster frame for the row -- and Windows will not
    // delete a file another handle still holds. The copies are in the system
    // temp folder and this test starts by clearing them.
    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    QApplication::processEvents();
    zaro::app::testing::discard(dropRoot);
}

// A file imported into a session that is already running.
//
// The regression: the media source resolves every path in the project once, as
// it opens, and knows nothing about what arrives afterwards. Importing through
// the pane -- the Import button, and a drop from the file manager -- left the
// file in the project with no decoder behind it, so the bin listed it and both
// monitors stayed black on it. Opening a project worked, which is why this
// survived: everything a project is opened with is resolved at that moment.
//
// Deliberately no `reopenMedia()` here. That call is the thing that used to be
// missing, and a test that made it would pass against the bug.
TEST_CASE("Media imported into a running session can be decoded", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;

    const std::filesystem::path importRoot =
        std::filesystem::temp_directory_path() / "zaro-selftest-late-import";
    zaro::app::testing::discard(importRoot);
    std::filesystem::create_directories(importRoot);
    const std::filesystem::path arrival = importRoot / "ARRIVED_LATE.mov";
    std::filesystem::copy_file(zaro::app::testing::mediaFixture("shaky_texture.mov"), arrival);

    auto* bin = window.bin();
    if (bin == nullptr) {
        zaro::app::testing::failf("there is no media pane\n");
    }
    const std::size_t before = window.project().media().size();
    if (bin->importPaths({QString::fromStdString(arrival.string())}) != 1 ||
        window.project().media().size() != before + 1) {
        zaro::app::testing::failf("the file did not import\n");
    }

    // Read afresh: reopening the media replaces the source, so a reference
    // taken before the import would be to the one that never knew about it.
    const auto arrived = window.project().media().back().id;
    if (!window.frameSource().imageFor(arrived,
                                       zaro::time::RationalTime{1, zaro::time::rates::fps25})) {
        zaro::app::testing::failf("nothing can decode a file imported since the session began\n");
    }

    // And the source monitor, which is where somebody looks at what they have
    // just imported. It renders through the provider the window handed it, so
    // it goes black on a file the provider cannot read.
    window.sourceMonitor()->load(*window.project().findMedia(arrived));
    QApplication::processEvents();
    if (window.sourceMonitor()->media() != arrived) {
        zaro::app::testing::failf("the source monitor did not open the imported file\n");
    }

    std::printf("  late import: %s decodes\n", arrival.filename().string().c_str());

    // Undone and reopened before the copy is deleted, not after: the decoder
    // this test just proved exists is holding the file open, and Windows will
    // not delete a file that something has open.
    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    window.sourceMonitor()->load(window.project().media().front());
    QApplication::processEvents();
    zaro::app::testing::discard(importRoot);
}

// A row in the media pane shows a frame of its file.
//
// Measured off the list itself rather than off the cache, because what was
// missing was not a decoder -- the timeline has drawn filmstrips from this same
// cache all along -- but a row that asks for a frame and draws it. The frame
// arrives on a worker thread, so the check waits for it the way the panel does:
// paint, let the loop run, paint again.
TEST_CASE("Media pane rows show a frame of the file", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;

    const std::filesystem::path posterRoot =
        std::filesystem::temp_directory_path() / "zaro-selftest-poster";
    zaro::app::testing::discard(posterRoot);
    std::filesystem::create_directories(posterRoot);
    // Texture rather than the fixture clip: `sync_click_flash.mov` is black
    // except on its flash frames, and a black poster frame is indistinguishable
    // from no poster frame at all.
    const std::filesystem::path textured = posterRoot / "TEXTURED.mov";
    std::filesystem::copy_file(zaro::app::testing::mediaFixture("shaky_texture.mov"), textured);

    auto* bin = window.bin();
    if (bin == nullptr || bin->importPaths({QString::fromStdString(textured.string())}) != 1) {
        zaro::app::testing::failf("the textured file did not import\n");
    }
    auto* list = bin->findChild<QListWidget*>("bin-list");
    if (list == nullptr) {
        zaro::app::testing::failf("the pane has no list\n");
    }

    const auto imported = window.project().media().back().id;
    QListWidgetItem* row = nullptr;
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->data(Qt::UserRole).toULongLong() == imported.value()) {
            row = list->item(i);
            break;
        }
    }
    if (row == nullptr) {
        zaro::app::testing::failf("the imported file has no row in the pane\n");
    }
    list->scrollToItem(row);
    QApplication::processEvents();

    // Where the delegate draws the picture: the plate is the row inset by half
    // the gutter, and the thumbnail sits six pixels inside it.
    const QRect rowRect = list->visualItemRect(row);
    const QRect plate{rowRect.left() + 9, rowRect.top() + (rowRect.height() - 38) / 2 + 1, 64, 38};
    // The upper-left of the picture, which is picture and nothing else: the
    // running time is a near-white badge in the bottom-right corner of the same
    // plate, and a box that included it would be satisfied by the placeholder.
    const QRect thumb{plate.left() + 12, plate.top() + 4, 33, 22};

    // The plate the row falls back to is dark and nearly flat; a frame of this
    // file runs from black to white. Waiting for the worker, not for a fixed
    // time: how long a decode takes is a property of the machine.
    int brightest = 0;
    for (int spin = 0; spin < 400 && brightest < 180; ++spin) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        QImage shot{list->viewport()->size(), QImage::Format_ARGB32};
        shot.fill(Qt::black);
        list->viewport()->render(&shot);
        brightest = 0;
        for (int y = thumb.top(); y < thumb.bottom() && y < shot.height(); ++y) {
            for (int x = thumb.left(); x < thumb.right() && x < shot.width(); ++x) {
                brightest = std::max(brightest, qGray(shot.pixel(x, y)));
            }
        }
    }
    if (brightest < 180) {
        zaro::app::testing::failf("the row's picture never arrived (brightest pixel %d)\n",
                                  brightest);
    }
    std::printf("  media pane poster: brightest pixel %d in the row's thumbnail\n", brightest);

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    if (Status reopened = window.reopenMedia(); !reopened) {
        zaro::app::testing::failf("%s\n", reopened.error().toString().c_str());
    }
    QApplication::processEvents();
    zaro::app::testing::discard(posterRoot);
}
