// Audio: the mixer, the meters, loudness and the processing chain.
//
// Driven through the real window against the real compositor. See GuiFixture.h
// for what is shared and why.

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <cmath>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/io/ProjectIo.h"

#include "../AudioStrip.h"
#include "../EffectControls.h"
#include "../FrameGrab.h"
#include "GuiFixture.h"

// The suite was written inside main(), which had this at file scope; the
// bodies still say `model::` and `Status` unqualified.
using namespace zaro;

using zaro::app::dragOnTimeline;
using zaro::app::settledGrab;

// Roles and auto-ducking, through the real panel.
//
// The fixture is a click track under a picture, which is not speech --
// so this block puts the audio clip in both roles in turn and checks
// that the answer depends on the role rather than on the file.
TEST_CASE("Roles and auto-ducking", "[gui]") {
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

    const auto duckSequenceId = window.project().activeSequence();
    const auto* duckSequence = window.project().findSequence(duckSequenceId);
    if (duckSequence->audioTracks().empty()) {
        zaro::app::testing::failf("no audio track to duck on\n");
    }
    const auto duckTrackId = duckSequence->audioTracks().front().id();
    const auto* duckTrack = window.project().findSequence(duckSequenceId)->findTrack(duckTrackId);
    if (duckTrack->clips().empty()) {
        zaro::app::testing::failf("no audio clip to duck\n");
    }
    const auto duckClipId = duckTrack->clips().front().id;

    timeline->selectOnly(duckTrackId, duckClipId);
    window.effects()->setSelection(duckTrackId, duckClipId);
    QApplication::processEvents();

    auto* roleBox = window.effects()->findChild<QComboBox*>("audio-role");
    auto* duckButton = window.effects()->findChild<QPushButton*>("duck-under-dialogue");
    if (roleBox == nullptr || duckButton == nullptr) {
        zaro::app::testing::failf("the audio role controls are not in the panel\n");
    }

    // As dialogue, the button is off: a clip cannot duck under itself.
    roleBox->setCurrentIndex(roleBox->findData(static_cast<int>(zaro::model::AudioRole::Dialogue)));
    QApplication::processEvents();
    const auto* asDialogue =
        window.project().findSequence(duckSequenceId)->findTrack(duckTrackId)->find(duckClipId);
    if (asDialogue->role != zaro::model::AudioRole::Dialogue) {
        zaro::app::testing::failf("the role did not reach the clip\n");
    }
    if (duckButton->isEnabled()) {
        zaro::app::testing::failf("dialogue was offered the chance to duck itself\n");
    }

    // Now give it something to duck under: a second copy of the same
    // audio on its own track, called dialogue. The file does not have
    // to be speech -- what the analysis reads is the level, and what
    // decides is the role.
    auto addedTrack = zaro::edit::makeAddTrack(window.project(), duckSequenceId,
                                               zaro::model::TrackKind::Audio, "A2");
    if (!addedTrack) {
        zaro::app::testing::failf("%s\n", addedTrack.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*addedTrack));
    const auto voiceTrackId =
        window.project().findSequence(duckSequenceId)->audioTracks().back().id();

    zaro::model::Clip voice =
        *window.project().findSequence(duckSequenceId)->findTrack(duckTrackId)->find(duckClipId);
    voice.id = window.project().ids().next<zaro::model::ClipTag>();
    voice.role = zaro::model::AudioRole::Dialogue;
    voice.animation = {};
    auto placedVoice =
        zaro::edit::makeOverwrite(window.project(), {duckSequenceId, voiceTrackId}, voice);
    if (!placedVoice) {
        zaro::app::testing::failf("%s\n", placedVoice.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*placedVoice));

    // And the original becomes the bed.
    timeline->selectOnly(duckTrackId, duckClipId);
    window.effects()->setSelection(duckTrackId, duckClipId);
    QApplication::processEvents();
    roleBox->setCurrentIndex(roleBox->findData(static_cast<int>(zaro::model::AudioRole::Music)));
    QApplication::processEvents();
    if (!duckButton->isEnabled()) {
        zaro::app::testing::failf("a music bed cannot be ducked\n");
    }
    duckButton->click();
    QApplication::processEvents();

    const auto* ducked =
        window.project().findSequence(duckSequenceId)->findTrack(duckTrackId)->find(duckClipId);
    const zaro::model::Curve* gain =
        ducked != nullptr ? ducked->animation.find(zaro::model::Param::GainDb) : nullptr;
    if (gain == nullptr || gain->size() < 2) {
        zaro::app::testing::failf("ducking under dialogue wrote no automation\n");
    }
    double lowest = 0.0;
    double highest = -1000.0;
    for (const zaro::model::Keyframe& key : gain->keyframes()) {
        lowest = std::min(lowest, key.value);
        highest = std::max(highest, key.value);
    }
    std::printf("  ducking: %zu keyframes between %.1f and %.1f dB\n", gain->size(), lowest,
                highest);
    if (!(lowest < -6.0) || !(highest > -1.0)) {
        zaro::app::testing::failf("the curve does not dip and come back\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
}

// Loudness, measured through the real mix and then normalised. The
// meter is tested headlessly against the standard's calibration case;
// what that cannot show is whether measuring a sequence and acting on
// the answer actually moves the programme.
TEST_CASE("Loudness, measured and normalised", "[gui]") {
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

    zaro::render::AudioGraph loudnessMix{window.media()};
    const zaro::time::TimeRange whole{zaro::time::RationalTime{0, sequence.frameRate()},
                                      window.sequence()->duration()};
    auto wasAt = loudnessMix.measureLoudness(*window.sequence(), whole);
    if (!wasAt) {
        zaro::app::testing::failf("%s\n", wasAt.error().toString().c_str());
    }
    if (!(wasAt->integratedLufs > zaro::render::LoudnessMeter::kSilence)) {
        zaro::app::testing::failf("the sequence measured as silence\n");
    }

    constexpr double kTarget = -23.0;
    const double gain = wasAt->gainToReach(kTarget);
    const auto audioTrack = window.project().findSequence(sequence.id())->audioTracks().front();
    zaro::edit::TrackState state;
    state.gainDb = audioTrack.gainDb() + gain;
    auto built =
        zaro::edit::makeSetTrackState(window.project(), sequence.id(), audioTrack.id(), state);
    if (!built) {
        zaro::app::testing::failf("%s\n", built.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*built));

    zaro::render::AudioGraph afterMix{window.media()};
    auto now = afterMix.measureLoudness(*window.sequence(), whole);
    if (!now) {
        zaro::app::testing::failf("%s\n", now.error().toString().c_str());
    }
    std::printf("  loudness: %.1f LUFS, %+.1f dB applied, now %.1f LUFS\n", wasAt->integratedLufs,
                gain, now->integratedLufs);
    if (std::fabs(now->integratedLufs - kTarget) > 0.5) {
        zaro::app::testing::failf("normalising did not land on the target\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.mixer()->refresh();
}

// The processing chain, through the mixer. The filters and the
// compressor are tested headlessly against known responses; what that
// cannot show is whether the strip's buttons reach the mix.
TEST_CASE("The processing chain, through the mixer", "[gui]") {
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

    const auto audioTrackId =
        window.project().findSequence(sequence.id())->audioTracks().front().id();
    // The EQ and compressor switches are on the channel strip beside the
    // console, not on the console itself. This looked for them on the mixer,
    // which is where they were before the chain got a panel of its own.
    window.setWorkspace("Audio");
    QApplication::processEvents();
    auto* eqBox = window.channel()->findChild<QCheckBox*>("eq-enabled");
    if (eqBox == nullptr) {
        zaro::app::testing::failf("the channel strip has no processing controls\n");
    }

    // A hard low pass: the fixture's audio is clicks, which are almost
    // entirely high frequency, so removing the top takes most of it.
    zaro::model::AudioEq eq;
    eq.enabled = true;
    eq.lowPassHz = 300.0;
    auto built = zaro::edit::makeSetTrackProcessing(window.project(), sequence.id(), audioTrackId,
                                                    eq, zaro::model::Compressor{});
    if (!built) {
        zaro::app::testing::failf("%s\n", built.error().toString().c_str());
    }

    const auto loudestNow = [&]() {
        zaro::render::AudioGraph probe{window.media()};
        float peak = 0.0F;
        const auto& audioRate = sequence.audioSampleRate();
        for (std::int64_t frame = 0; frame < 60; ++frame) {
            probe.resetProcessing();
            const zaro::time::RationalTime at{frame, sequence.frameRate()};
            if (auto mixed = probe.mix(*window.sequence(), at.rescaledTo(audioRate), 2048, 2)) {
                for (std::int64_t i = 0; i < mixed->sampleCount(); ++i) {
                    peak = std::max(peak, std::fabs(mixed->channel(0)[i]));
                }
            }
        }
        return peak;
    };

    const float plain = loudestNow();
    window.commands().execute(window.project(), std::move(*built));
    const float filtered = loudestNow();

    std::printf("  audio processing: %.3f plain, %.3f through a 300 Hz low pass\n",
                static_cast<double>(plain), static_cast<double>(filtered));
    if (!(plain > 0.05F)) {
        zaro::app::testing::failf("nothing to filter\n");
    }
    if (!(filtered < plain * 0.5F)) {
        zaro::app::testing::failf("the low pass did not reach the mix\n");
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.mixer()->refresh();
}

// The mixer: solo, and the meters. Solo is a rule about the whole
// sequence rather than a property of one track, so the check is that
// soloing one track silences another.
TEST_CASE("The mixer: solo, and the meters", "[gui]") {
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

    auto* audioTrack = window.project()
                           .findSequence(sequence.id())
                           ->tracksMutable(zaro::model::TrackKind::Audio)
                           .data();
    if (audioTrack == nullptr) {
        zaro::app::testing::failf("no audio track to mix\n");
    }
    // The mixer is up in the Audio workspace, which is the point of
    // workspaces: it is not on screen while somebody is assembling.
    window.setWorkspace("Audio");
    window.mixer()->refresh();
    QApplication::processEvents();

    // The meters are only updated while the panel is visible, which is
    // the right behaviour and a trap for a test: whether a widget has
    // become visible depends on how many event loops have run, so this
    // asks explicitly rather than depending on it. Left implicit it
    // read zero once in five runs and looked like a broken mixer.
    for (int i = 0; i < 5 && !window.mixer()->isVisible(); ++i) {
        QApplication::processEvents();
    }
    if (!window.mixer()->isVisible()) {
        zaro::app::testing::failf("the mixer panel never became visible\n");
    }

    // The meter is painted by the strip itself. It used to be a LevelMeter
    // child named "mixer-meter-<id>"; the strips took that over, and this
    // looked for the old names for as long as nothing ran it.
    auto* meter = window.mixer()->findChild<app::AudioStrip*>(
        QString{"mixer-strip-"} + QString::number(audioTrack->id().value()));
    auto* master = window.mixer()->findChild<app::AudioStrip*>("mixer-master");
    if (meter == nullptr || master == nullptr) {
        zaro::app::testing::failf("the mixer has no meters\n");
    }

    // This fixture's audio is clicks a second apart, so most positions
    // are silence -- and a peak hold is designed to keep showing the
    // last loud thing, so reading it at one arbitrary position gives
    // either the click or whatever was held from somewhere else. Scan
    // instead, and take the loudest.
    const auto loudest = [&](app::AudioStrip* which) {
        float peak = 0.0F;
        for (std::int64_t frame = 0; frame < 60; ++frame) {
            which->resetHold();
            window.setPosition(zaro::time::RationalTime{frame, sequence.frameRate()});
            QApplication::processEvents();
            window.updateMeters();
            peak = std::max(peak, which->hold());
        }
        return peak;
    };
    const float heard = loudest(meter);
    const float masterHeard = loudest(master);

    // Soloing a *video* track must leave the mix exactly as it was. This
    // used to silence everything: solo was asked across both lists at once,
    // so isolating a shot -- an ordinary thing to do while cutting -- muted
    // the whole sequence, and an export with that solo still set came out
    // with a perfect picture and no sound at all.
    auto* videoSolo = window.project()
                          .findSequence(sequence.id())
                          ->tracksMutable(zaro::model::TrackKind::Video)
                          .data();
    zaro::edit::TrackState soloed;
    soloed.soloed = true;
    auto isolatedPicture =
        zaro::edit::makeSetTrackState(window.project(), sequence.id(), videoSolo->id(), soloed);
    if (!isolatedPicture) {
        zaro::app::testing::failf("%s\n", isolatedPicture.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*isolatedPicture));
    window.mixer()->refresh();
    const float pictureSoloed = loudest(meter);
    window.commands().undo(window.project());
    window.mixer()->refresh();

    // Solo a second *sound* track: this one is not soloed, so it falls silent
    // even though nobody muted it. Picture and sound are separate solo groups,
    // so it takes a sound track to silence a sound track.
    auto addTrack = zaro::edit::makeAddTrack(window.project(), sequence.id(),
                                             zaro::model::TrackKind::Audio, "A-solo");
    if (!addTrack) {
        zaro::app::testing::failf("%s\n", addTrack.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*addTrack));
    const auto otherAudio = window.project().findSequence(sequence.id())->audioTracks().back().id();
    auto built = zaro::edit::makeSetTrackState(window.project(), sequence.id(), otherAudio, soloed);
    if (!built) {
        zaro::app::testing::failf("%s\n", built.error().toString().c_str());
    }
    window.commands().execute(window.project(), std::move(*built));
    window.mixer()->refresh();
    const float silenced = loudest(meter);

    std::printf(
        "  mixer meters: %.3f heard, %.3f once another sound track is soloed, "
        "%.3f with a video track soloed (master %.3f)\n",
        static_cast<double>(heard), static_cast<double>(silenced),
        static_cast<double>(pictureSoloed), static_cast<double>(masterHeard));
    if (!(heard > 0.01F)) {
        zaro::app::testing::failf("the meters read nothing on a clip with sound\n");
    }
    if (!(masterHeard > 0.01F)) {
        zaro::app::testing::failf("the master meter reads nothing\n");
    }
    if (!(silenced < heard * 0.05F)) {
        zaro::app::testing::failf("soloing another sound track did not silence this one\n");
    }
    if (!(pictureSoloed > heard * 0.5F)) {
        zaro::app::testing::failf("soloing a video track silenced the mix: %.3f, was %.3f\n",
                                  static_cast<double>(pictureSoloed), static_cast<double>(heard));
    }

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
    window.mixer()->refresh();
}

// Per-clip filtering and compression: the repair a track's channel strip
// cannot do, because a track's applies to everything on it.
//
// `AudioEq` and `Compressor` were on `Track` alone until now, so the only way
// to hold one loud take down was to split it onto a track of its own — which
// is how a three-track mix becomes an eleven-track one.
TEST_CASE("A clip's own repair, set from the inspector", "[gui]") {
    auto& window = zaro::app::testing::gui();
    const zaro::app::testing::Rewind rewind;
    auto* panel = window.effects();
    const auto& sequence = *window.sequence();
    const auto sequenceId = sequence.id();
    const auto& audioTrack = sequence.audioTracks().front();
    if (audioTrack.clips().empty()) {
        zaro::app::testing::failf("the fixture has no sound clip\n");
    }
    const auto trackId = audioTrack.id();
    const auto clipId = audioTrack.clips().front().id;

    panel->setSelection(trackId, clipId);
    panel->setPane(zaro::app::EffectControls::Pane::Audio);
    QApplication::processEvents();

    auto* repair = panel->findChild<QGroupBox*>("inspector-group-processing");
    if (repair == nullptr || !repair->isVisibleTo(panel)) {
        zaro::app::testing::failf("a sound clip is not offered any repair\n");
    }

    auto* filterOn = panel->findChild<QCheckBox*>("clip-eq-on");
    auto* highPass = panel->findChild<QDoubleSpinBox*>("clip-highpass");
    auto* compressOn = panel->findChild<QCheckBox*>("clip-compressor-on");
    auto* ratio = panel->findChild<QDoubleSpinBox*>("clip-ratio");
    if (filterOn == nullptr || highPass == nullptr || compressOn == nullptr || ratio == nullptr) {
        zaro::app::testing::failf("the repair group is missing its controls\n");
    }

    const auto clipNow = [&]() -> const zaro::model::Clip* {
        return window.project().findSequence(sequenceId)->findTrack(trackId)->find(clipId);
    };
    if (clipNow()->eq.enabled || clipNow()->compressor.enabled) {
        zaro::app::testing::failf("a clip arrives already processed\n");
    }

    filterOn->setChecked(true);
    highPass->setValue(120.0);
    compressOn->setChecked(true);
    ratio->setValue(6.0);
    QApplication::processEvents();

    if (!clipNow()->eq.enabled || std::fabs(clipNow()->eq.highPassHz - 120.0) > 1e-6) {
        zaro::app::testing::failf("the filter did not reach the clip (%.1f Hz, %s)\n",
                                  clipNow()->eq.highPassHz, clipNow()->eq.enabled ? "on" : "off");
    }
    if (!clipNow()->compressor.enabled || std::fabs(clipNow()->compressor.ratio - 6.0) > 1e-6) {
        zaro::app::testing::failf("the compressor did not reach the clip\n");
    }
    // The track it sits on is untouched: that is the whole point of the clip
    // having its own.
    const auto& track = *window.project().findSequence(sequenceId)->findTrack(trackId);
    if (track.eq().enabled || track.compressor().enabled) {
        zaro::app::testing::failf("repairing a clip processed its whole track\n");
    }

    // It survives a save, which is what makes it a repair rather than a
    // preview.
    const auto text = zaro::io::saveProjectToString(window.project());
    if (!text) {
        zaro::app::testing::failf("could not save\n");
    }
    const auto reloaded = zaro::io::loadProjectFromString(*text);
    if (!reloaded) {
        zaro::app::testing::failf("could not load back\n");
    }
    const zaro::model::Clip* saved =
        reloaded->project.findSequence(sequenceId)->findTrack(trackId)->find(clipId);
    if (saved == nullptr || !saved->eq.enabled || std::fabs(saved->eq.highPassHz - 120.0) > 1e-6) {
        zaro::app::testing::failf("the repair did not survive a save\n");
    }

    window.commands().undo(window.project());
    panel->refresh();
    QApplication::processEvents();
    if (std::fabs(clipNow()->compressor.ratio - 6.0) < 1e-6 && clipNow()->compressor.enabled) {
        zaro::app::testing::failf("undo did not take the last change back\n");
    }

    // And a picture clip is not offered any of it: the Audio page has nothing
    // behind it for one.
    const auto videoTrackId = window.sequence()->videoTracks().front().id();
    const auto videoClipId = window.sequence()->videoTracks().front().clips().front().id;
    panel->setSelection(videoTrackId, videoClipId);
    QApplication::processEvents();
    if (repair->isVisibleTo(panel)) {
        zaro::app::testing::failf("a picture clip is offered audio repair\n");
    }

    std::printf("  clip repair: 120 Hz high pass and 6:1, on the clip and not the track\n");

    while (window.commands().canUndo()) {
        window.commands().undo(window.project());
    }
}
