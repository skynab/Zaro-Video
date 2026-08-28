#include <cmath>
#include <cstdint>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/edit/Sync.h"
#include "zaro/core/media/AudioAlign.h"

#include "ModelFixtures.h"

using namespace zaro;
using Catch::Approx;
using zaro::testing::Fixture;

namespace {

constexpr std::int64_t kRate = 48000;

/// Deterministic broadband noise: the same sample index always gives the same
/// value, so two "recordings" of it are two views of one signal.
float signalAt(std::int64_t n) {
    if (n < 0) {
        return 0.0F;
    }
    auto state = static_cast<std::uint64_t>(n) * 6364136223846793005ULL + 1442695040888963407ULL;
    state ^= state >> 33;
    state *= 0xff51afd7ed558ccdULL;
    state ^= state >> 33;
    const auto unit = static_cast<double>(state >> 11) / static_cast<double>(1ULL << 53);
    // Bursts with gaps, so there is a shape to line up rather than a wash that
    // correlates with itself everywhere.
    const bool loud = ((n / (kRate / 2)) % 3) == 0;
    return static_cast<float>(((unit * 2.0) - 1.0) * (loud ? 1.0 : 0.02));
}

std::vector<float> recording(std::int64_t count, std::int64_t delay, double level = 1.0) {
    std::vector<float> out(static_cast<std::size_t>(count));
    for (std::int64_t n = 0; n < count; ++n) {
        out[static_cast<std::size_t>(n)] =
            static_cast<float>(static_cast<double>(signalAt(n - delay)) * level);
    }
    return out;
}

media::AlignOptions options(std::int64_t maxLag) {
    media::AlignOptions out;
    out.maxLagSamples = maxLag;
    out.blockSamples = kRate / 100;
    out.refineWindowSamples = kRate;
    return out;
}

media::Alignment alignOf(const std::vector<float>& a, const std::vector<float>& b,
                         std::int64_t maxLag = kRate * 2) {
    return media::align(a.data(), static_cast<std::int64_t>(a.size()), b.data(),
                        static_cast<std::int64_t>(b.size()), options(maxLag));
}

/// An audio source where each media is the same room, recorded from a
/// different moment.
class DelayedAudioSource final : public render::AudioSource {
public:
    void define(model::MediaRefId media, std::int64_t delaySamples) {
        delays_[media.value()] = delaySamples;
    }

    Status read(model::MediaRefId media, const time::RationalTime& sourceStart,
                std::int64_t sampleCount, const time::Rational& sampleRate,
                media::AudioBuffer& out) override {
        const auto found = delays_.find(media.value());
        if (found == delays_.end()) {
            return Error{ErrorCode::NotFound, "no such media in this test source"};
        }
        const std::int64_t start = sourceStart.rescaledTo(sampleRate).frames();
        out = media::AudioBuffer{2, sampleCount, sampleRate};
        for (std::int32_t channel = 0; channel < 2; ++channel) {
            float* samples = out.channel(channel);
            for (std::int64_t i = 0; i < sampleCount; ++i) {
                samples[i] = signalAt(start + i - found->second);
            }
        }
        return {};
    }

private:
    std::map<std::uint64_t, std::int64_t> delays_;
};

/// Give a media reference a start timecode, the way a probe would have.
void setStartTimecode(model::Project& project, model::MediaRefId id, const char* text) {
    for (model::MediaRef& media : project.mediaMutable()) {
        if (media.id == id) {
            media.info.videoStreams.front().startTimecode = time::parseTimecode(text);
        }
    }
}

std::vector<model::Clip::Angle> angles(const std::vector<model::MediaRefId>& media) {
    std::vector<model::Clip::Angle> out;
    for (std::size_t i = 0; i < media.size(); ++i) {
        model::Clip::Angle angle;
        angle.media = media[i];
        angle.name = std::string{"cam "} + static_cast<char>('A' + static_cast<char>(i));
        out.push_back(angle);
    }
    return out;
}

}  // namespace

// --- The signal work --------------------------------------------------------

TEST_CASE("A recording lines up with itself", "[media][align]") {
    const auto a = recording(kRate * 4, 0);
    const media::Alignment found = alignOf(a, a);
    CHECK(found.reason.empty());
    CHECK(found.offsetSamples == 0);
    CHECK(found.confidence > 0.99);
}

TEST_CASE("A delayed recording is found to the sample", "[media][align]") {
    // Not "to within a block". A tenth of a frame out is the sync error nobody
    // finds later, because it looks right on most cuts.
    const std::int64_t delay =
        GENERATE(std::int64_t{1}, std::int64_t{137}, std::int64_t{4801}, std::int64_t{-2400});
    const auto reference = recording(kRate * 6, 0);
    const auto other = recording(kRate * 6, delay);

    const media::Alignment found = alignOf(reference, other);
    CHECK(found.reason.empty());
    CHECK(found.offsetSamples == delay);
    CHECK(found.confidence > 0.9);
}

TEST_CASE("Level does not decide the answer", "[media][align]") {
    // Two cameras at different distances from the same clap record the same
    // shape at different levels; it is the shape that says where they line up.
    const auto reference = recording(kRate * 4, 0);
    const auto quiet = recording(kRate * 4, 900, 0.05);

    const media::Alignment found = alignOf(reference, quiet);
    CHECK(found.offsetSamples == 900);
    CHECK(found.confidence > 0.9);
}

TEST_CASE("Silence is said to be silence", "[media][align]") {
    const auto reference = recording(kRate * 2, 0);
    const std::vector<float> silence(static_cast<std::size_t>(kRate * 2), 0.0F);

    const media::Alignment found = alignOf(reference, silence);
    CHECK_FALSE(found.reason.empty());
    CHECK(found.confidence == 0.0);
    // And no confident zero to mistake for an answer.
    CHECK(found.offsetSamples == 0);
}

TEST_CASE("Two different rooms are not synced", "[media][align]") {
    std::vector<float> reference(static_cast<std::size_t>(kRate * 4));
    std::vector<float> other(static_cast<std::size_t>(kRate * 4));
    for (std::int64_t n = 0; n < kRate * 4; ++n) {
        reference[static_cast<std::size_t>(n)] = signalAt(n);
        other[static_cast<std::size_t>(n)] = signalAt(n + 900000000);
    }
    const media::Alignment found = alignOf(reference, other);
    CHECK(found.confidence < 0.5);
}

TEST_CASE("An envelope is the loudness of each block", "[media][align]") {
    std::vector<float> samples(400, 0.0F);
    for (std::size_t i = 200; i < 400; ++i) {
        samples[i] = 0.5F;
    }
    const std::vector<double> levels = media::envelope(samples.data(), 400, 100);
    REQUIRE(levels.size() == 4);
    CHECK(levels[0] == Approx(0.0));
    CHECK(levels[2] == Approx(0.5));
}

// --- Timecode ---------------------------------------------------------------

TEST_CASE("Timecode syncs the angles by subtraction", "[edit][sync]") {
    Fixture f;
    const model::MediaRefId second = f.addMedia("cam-b.mov", 10000);
    setStartTimecode(f.project, f.longMedia, "01:00:00:00");
    // Started rolling two seconds later on the same clock.
    setStartTimecode(f.project, second, "01:00:02:00");

    REQUIRE(f.run(
        edit::makeMulticam(f.project, f.on(f.v1), angles({f.longMedia, second}), f.range(0, 100))));
    const model::Clip& clip = f.track(f.v1).clips().front();

    auto synced = edit::syncByTimecode(f.project, clip);
    REQUIRE(synced);
    REQUIRE(synced->size() == 2);
    CHECK((*synced)[0].offset.has_value());
    CHECK((*synced)[0].offset->toSeconds().toDouble() == Approx(0.0));
    // The later camera has to be read two seconds *earlier* into its own
    // material for its clock to read the same.
    CHECK((*synced)[1].offset->toSeconds().toDouble() == Approx(-2.0));
    CHECK((*synced)[1].confidence == 1.0);
}

TEST_CASE("Synced angles show the same moment", "[edit][sync]") {
    // The property the offsets exist for, checked through the clip rather than
    // through the arithmetic that produced them.
    Fixture f;
    const model::MediaRefId second = f.addMedia("cam-b.mov", 10000);
    setStartTimecode(f.project, f.longMedia, "01:00:00:00");
    setStartTimecode(f.project, second, "00:59:58:00");

    REQUIRE(f.run(
        edit::makeMulticam(f.project, f.on(f.v1), angles({f.longMedia, second}), f.range(0, 100))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;

    auto synced = edit::syncByTimecode(f.project, f.track(f.v1).clips().front());
    REQUIRE(synced);
    std::vector<std::pair<std::int32_t, time::RationalTime>> offsets;
    for (const edit::AngleSync& entry : *synced) {
        if (entry.offset.has_value()) {
            offsets.emplace_back(entry.angle, *entry.offset);
        }
    }
    REQUIRE(f.run(edit::makeSetAngleOffsets(f.project, f.on(f.v1), clipId, offsets)));

    // The same instant on the timeline, read through each angle, lands on the
    // same timecode in the two files.
    model::Clip first = f.track(f.v1).clips().front();
    model::Clip other = first;
    other.activeAngle = 1;
    const time::RationalTime when = f.at(37);
    const time::RationalTime inA = first.activeSourceTimeAt(when);
    const time::RationalTime inB = other.activeSourceTimeAt(when);
    // Camera B's file starts two seconds earlier on the clock, so the same
    // moment is two seconds further into it.
    CHECK((inB - inA).toSeconds().toDouble() == Approx(2.0));
}

TEST_CASE("An angle without timecode is left alone and said so", "[edit][sync]") {
    Fixture f;
    const model::MediaRefId second = f.addMedia("cam-b.mov", 10000);
    const model::MediaRefId third = f.addMedia("cam-c.mov", 10000);
    setStartTimecode(f.project, f.longMedia, "01:00:00:00");
    setStartTimecode(f.project, second, "01:00:05:00");
    // Nothing on the third.

    REQUIRE(f.run(edit::makeMulticam(f.project, f.on(f.v1), angles({f.longMedia, second, third}),
                                     f.range(0, 100))));
    auto synced = edit::syncByTimecode(f.project, f.track(f.v1).clips().front());
    REQUIRE(synced);
    REQUIRE(synced->size() == 3);
    CHECK((*synced)[1].offset.has_value());
    // One camera that was not jam-synced does not stop the other two.
    CHECK_FALSE((*synced)[2].offset.has_value());
    CHECK_FALSE((*synced)[2].reason.empty());
}

TEST_CASE("Syncing needs somewhere to sync to", "[edit][sync]") {
    Fixture f;
    const model::MediaRefId second = f.addMedia("cam-b.mov", 10000);

    SECTION("no timecode on the reference") {
        REQUIRE(f.run(edit::makeMulticam(f.project, f.on(f.v1), angles({f.longMedia, second}),
                                         f.range(0, 100))));
        CHECK_FALSE(edit::syncByTimecode(f.project, f.track(f.v1).clips().front()));
    }

    SECTION("only one angle") {
        model::Clip plain = f.clip(0, 50);
        REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), plain)));
        CHECK_FALSE(edit::syncByTimecode(f.project, f.track(f.v1).clips().front()));
    }
}

// --- Audio ------------------------------------------------------------------

TEST_CASE("Audio syncs the angles by ear", "[edit][sync]") {
    Fixture f;
    const model::MediaRefId second = f.addMedia("cam-b.mov", 10000);
    REQUIRE(f.run(
        edit::makeMulticam(f.project, f.on(f.v1), angles({f.longMedia, second}), f.range(0, 100))));

    DelayedAudioSource audio;
    audio.define(f.longMedia, 0);
    // Camera B started rolling 1.5 seconds after camera A, so its file is
    // missing the first second and a half.
    audio.define(second, -kRate * 3 / 2);

    edit::AudioSyncOptions opts;
    opts.window = time::RationalTime{kRate * 8, time::Rational{kRate, 1}};
    opts.maxOffset = time::RationalTime{kRate * 3, time::Rational{kRate, 1}};

    auto synced = edit::syncByAudio(f.project, f.track(f.v1).clips().front(), audio, opts);
    REQUIRE(synced);
    REQUIRE(synced->size() == 2);
    CHECK((*synced)[0].offset->frames() == 0);
    REQUIRE((*synced)[1].offset.has_value());
    // A later start means reading *less* far into that angle's own material
    // for the same moment, which is a negative offset.
    CHECK((*synced)[1].offset->toSeconds().toDouble() == Approx(-1.5).margin(0.001));
    CHECK((*synced)[1].confidence > 0.9);
}

TEST_CASE("An angle that sounds like nothing else is refused, not guessed", "[edit][sync]") {
    Fixture f;
    const model::MediaRefId second = f.addMedia("cam-b.mov", 10000);
    REQUIRE(f.run(
        edit::makeMulticam(f.project, f.on(f.v1), angles({f.longMedia, second}), f.range(0, 100))));

    DelayedAudioSource audio;
    audio.define(f.longMedia, 0);
    // Far enough out that the search cannot reach it: the honest answer is
    // "not found", not the best of a bad set of lags.
    audio.define(second, -kRate * 20);

    edit::AudioSyncOptions opts;
    opts.window = time::RationalTime{kRate * 8, time::Rational{kRate, 1}};
    opts.maxOffset = time::RationalTime{kRate * 2, time::Rational{kRate, 1}};

    auto synced = edit::syncByAudio(f.project, f.track(f.v1).clips().front(), audio, opts);
    REQUIRE(synced);
    CHECK_FALSE((*synced)[1].offset.has_value());
    CHECK_FALSE((*synced)[1].reason.empty());
}

// --- Writing it back --------------------------------------------------------

TEST_CASE("Setting the offsets is one undoable step", "[edit][sync]") {
    Fixture f;
    const model::MediaRefId second = f.addMedia("cam-b.mov", 10000);
    REQUIRE(f.run(
        edit::makeMulticam(f.project, f.on(f.v1), angles({f.longMedia, second}), f.range(0, 100))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;

    REQUIRE(f.run(edit::makeSetAngleOffsets(f.project, f.on(f.v1), clipId, {{1, f.at(25)}})));
    CHECK(f.track(f.v1).clips().front().angles[1].offset.frames() == 25);

    f.stack.undo(f.project);
    CHECK(f.track(f.v1).clips().front().angles[1].offset.frames() == 0);

    SECTION("and an angle that does not exist is refused") {
        CHECK_FALSE(
            f.run(edit::makeSetAngleOffsets(f.project, f.on(f.v1), clipId, {{7, f.at(1)}})));
    }
}
