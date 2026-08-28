#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"
#include "zaro/core/render/Ducking.h"

#include "ModelFixtures.h"

using namespace zaro;
using Catch::Approx;
using zaro::testing::Fixture;

namespace {

const time::Rational kAudioRate{48000, 1};

/// An audio source where one media is speech for part of its length and room
/// tone for the rest, and another is a steady tone.
class SpeechSource final : public render::AudioSource {
public:
    /// Loud from `fromSeconds` to `toSeconds`, near-silent either side.
    void defineSpeech(model::MediaRefId media, double fromSeconds, double toSeconds) {
        speech_[media.value()] = {fromSeconds, toSeconds};
    }
    void defineSteady(model::MediaRefId media, float level) { steady_[media.value()] = level; }

    Status read(model::MediaRefId media, const time::RationalTime& sourceStart,
                std::int64_t sampleCount, const time::Rational& sampleRate,
                media::AudioBuffer& out) override {
        out = media::AudioBuffer{2, sampleCount, sampleRate};
        const double start = static_cast<double>(sourceStart.rescaledTo(sampleRate).frames()) /
                             static_cast<double>(sampleRate.toDouble());
        const auto found = speech_.find(media.value());
        const auto tone = steady_.find(media.value());
        for (std::int32_t channel = 0; channel < 2; ++channel) {
            float* samples = out.channel(channel);
            for (std::int64_t i = 0; i < sampleCount; ++i) {
                const double at = start + (static_cast<double>(i) / sampleRate.toDouble());
                float value = 0.0F;
                if (found != speech_.end()) {
                    const bool loud = at >= found->second.first && at < found->second.second;
                    // Alternating sign so the RMS is the amplitude rather than
                    // a DC offset the envelope would read the same way.
                    value = (loud ? 0.5F : 0.001F) * ((i % 2) == 0 ? 1.0F : -1.0F);
                } else if (tone != steady_.end()) {
                    value = tone->second * ((i % 2) == 0 ? 1.0F : -1.0F);
                }
                samples[i] = value;
            }
        }
        return {};
    }

private:
    std::map<std::uint64_t, std::pair<double, double>> speech_;
    std::map<std::uint64_t, float> steady_;
};

/// A project with music on A1 and dialogue on A2, both eight seconds long.
struct DuckFixture : Fixture {
    SpeechSource audio;
    model::TrackId a2;
    model::MediaRefId musicMedia;
    model::MediaRefId voiceMedia;
    model::ClipId musicId;

    DuckFixture() {
        a2 = project.ids().next<model::TrackTag>();
        sequence().addTrack(a2, model::TrackKind::Audio, "A2");
        musicMedia = addMedia("music.wav", 10000);
        voiceMedia = addMedia("voice.wav", 10000);
        audio.defineSteady(musicMedia, 0.3F);
        // Speech from two seconds to four, silence either side.
        audio.defineSpeech(voiceMedia, 2.0, 4.0);

        model::Clip music = clip(0, 200, 0, musicMedia);
        music.role = model::AudioRole::Music;
        musicId = music.id;
        REQUIRE(run(edit::makeOverwrite(project, on(a1), music)));

        model::Clip voice = clip(0, 200, 0, voiceMedia);
        voice.role = model::AudioRole::Dialogue;
        REQUIRE(run(edit::makeOverwrite(project, on(a2), voice)));
    }

    [[nodiscard]] const model::Clip& music() const { return *track(a1).find(musicId); }

    /// The curve's value at a moment on the timeline.
    [[nodiscard]] double gainAt(const model::Curve& curve, std::int64_t frame) const {
        return curve.valueAtSeconds(music().sourceSecondsAt(at(frame)));
    }
};

}  // namespace

TEST_CASE("Ducking follows what is heard, not what is on the timeline", "[render][ducking]") {
    // The dialogue clip covers the whole eight seconds; somebody speaks for two
    // of them. Ducking for the other six would be following the cut rather than
    // the sound.
    DuckFixture f;
    auto curve = render::duckingCurve(f.sequence(), f.music(), f.audio);
    REQUIRE(curve);
    REQUIRE_FALSE(curve->empty());

    // At half a second nobody has spoken yet.
    CHECK(f.gainAt(*curve, 12) == Approx(0.0).margin(0.5));
    // Three seconds in, they are speaking.
    CHECK(f.gainAt(*curve, 75) == Approx(-12.0).margin(0.5));
    // Seven seconds in, they finished long ago.
    CHECK(f.gainAt(*curve, 175) == Approx(0.0).margin(0.5));
}

TEST_CASE("Ducking comes down quickly and goes back slowly", "[render][ducking]") {
    // Coming down late buries the first word; going back up early pumps under
    // the pause between sentences.
    DuckFixture f;
    render::DuckingOptions options;
    options.fadeDown = time::RationalTime{4800, kAudioRate};  // 0.1s
    options.fadeUp = time::RationalTime{48000, kAudioRate};   // 1.0s
    options.hold = time::RationalTime{0, kAudioRate};
    auto curve = render::duckingCurve(f.sequence(), f.music(), f.audio, options);
    REQUIRE(curve);

    // A tenth of a second before the speech starts it is still open; a tenth
    // after, it is down.
    CHECK(f.gainAt(*curve, 45) == Approx(0.0).margin(1.0));
    CHECK(f.gainAt(*curve, 53) == Approx(-12.0).margin(1.0));
    // A tenth of a second after the speech ends it is still down, because the
    // way back takes a second.
    CHECK(f.gainAt(*curve, 103) < -6.0);
}

TEST_CASE("A pause shorter than the hold does not lift the music", "[render][ducking]") {
    DuckFixture f;
    // Two bursts with half a second between them.
    f.audio.defineSpeech(f.voiceMedia, 2.0, 3.0);
    render::DuckingOptions options;
    options.hold = time::RationalTime{48000, kAudioRate};  // one second
    auto held = render::duckingCurve(f.sequence(), f.music(), f.audio, options);
    REQUIRE(held);
    // Still down a quarter of a second after the words stop.
    CHECK(f.gainAt(*held, 81) == Approx(-12.0).margin(1.0));

    SECTION("and with no hold it lifts straight away") {
        options.hold = time::RationalTime{0, kAudioRate};
        options.fadeUp = time::RationalTime{4800, kAudioRate};
        auto quick = render::duckingCurve(f.sequence(), f.music(), f.audio, options);
        REQUIRE(quick);
        CHECK(f.gainAt(*quick, 81) > -6.0);
    }
}

TEST_CASE("Ducking is a change to the level somebody set", "[render][ducking]") {
    // Not a replacement of it: a music bed already pulled down by six should
    // duck to eighteen, not to twelve.
    DuckFixture f;
    model::Clip& music = const_cast<model::Clip&>(f.music());
    music.gainDb = -6.0;

    auto curve = render::duckingCurve(f.sequence(), f.music(), f.audio);
    REQUIRE(curve);
    CHECK(f.gainAt(*curve, 12) == Approx(-6.0).margin(0.5));
    CHECK(f.gainAt(*curve, 75) == Approx(-18.0).margin(0.5));
}

TEST_CASE("Only dialogue ducks", "[render][ducking]") {
    DuckFixture f;
    // The same sound, classified as something else.
    for (const model::Clip& clip : f.track(f.a2).clips()) {
        REQUIRE(f.run(
            edit::makeSetAudioRole(f.project, f.on(f.a2), clip.id, model::AudioRole::Effects)));
    }
    // Nothing to duck under: a door slam is not somebody talking, and a mix
    // that dipped the music for one would be unusable.
    CHECK_FALSE(render::duckingCurve(f.sequence(), f.music(), f.audio));
}

TEST_CASE("A muted dialogue track does not duck", "[render][ducking]") {
    DuckFixture f;
    f.sequence().findTrack(f.a2)->setMuted(true);
    CHECK_FALSE(render::duckingCurve(f.sequence(), f.music(), f.audio));
}

TEST_CASE("Applying a duck is one undoable step", "[edit][ducking]") {
    DuckFixture f;
    auto curve = render::duckingCurve(f.sequence(), f.music(), f.audio);
    REQUIRE(curve);

    REQUIRE(
        f.run(edit::makeSetCurve(f.project, f.on(f.a1), f.musicId, model::Param::GainDb, *curve)));
    REQUIRE(f.music().animation.find(model::Param::GainDb) != nullptr);

    f.stack.undo(f.project);
    // The level somebody had, not two hundred keyframes removed one at a time.
    CHECK(f.music().animation.find(model::Param::GainDb) == nullptr);

    SECTION("and an empty curve clears it again") {
        REQUIRE(f.run(
            edit::makeSetCurve(f.project, f.on(f.a1), f.musicId, model::Param::GainDb, *curve)));
        REQUIRE(f.run(edit::makeSetCurve(f.project, f.on(f.a1), f.musicId, model::Param::GainDb,
                                         model::Curve{})));
        CHECK(f.music().animation.find(model::Param::GainDb) == nullptr);
    }
}

TEST_CASE("A role survives a round trip", "[edit][ducking][io]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 50))));
    const model::ClipId clipId = f.track(f.a1).clips().front().id;
    REQUIRE(
        f.run(edit::makeSetAudioRole(f.project, f.on(f.a1), clipId, model::AudioRole::Ambience)));

    auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    auto reloaded = io::loadProjectFromString(*text);
    REQUIRE(reloaded);
    CHECK(reloaded->project.findSequence(f.sequenceId)->findTrack(f.a1)->clips().front().role ==
          model::AudioRole::Ambience);

    SECTION("and an unassigned role is not written at all") {
        Fixture plain;
        REQUIRE(
            plain.run(edit::makeOverwrite(plain.project, plain.on(plain.a1), plain.clip(0, 50))));
        auto bare = io::saveProjectToString(plain.project);
        REQUIRE(bare);
        CHECK(bare->find("\"role\"") == std::string::npos);
    }
}
