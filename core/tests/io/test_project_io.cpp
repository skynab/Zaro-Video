#include <cstdint>
#include <set>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"

#include "ModelFixtures.h"

using namespace zaro;
using zaro::testing::Fixture;

namespace {

/// A project with enough shape that a round trip has something to lose.
Fixture populated() {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 600))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v2), f.clip(25, 100, 700))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 150, 800))));
    f.track(f.v2).setMuted(true);
    f.track(f.a1).setLocked(true);
    f.sequence().setStartTime(time::RationalTime{107892, time::rates::fps25});
    return f;
}

}  // namespace

TEST_CASE("A project survives a round trip unchanged", "[io]") {
    Fixture f = populated();

    const auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    const auto loaded = io::loadProjectFromString(*text);
    REQUIRE(loaded);

    CHECK(loaded->project == f.project);

    SECTION("and saving the loaded copy produces the identical bytes") {
        const auto again = io::saveProjectToString(loaded->project, loaded->unknown);
        REQUIRE(again);
        CHECK(*again == *text);
    }
}

TEST_CASE("Rates survive as exact fractions, not decimals", "[io]") {
    Fixture f;
    // Name deliberately free of digits: the assertion below is that no rounded
    // rate appears anywhere in the file, and a name would be a false positive.
    model::Sequence broadcast{f.project.ids().next<model::SequenceTag>(), "Broadcast master",
                              time::rates::fps29_97};
    const model::SequenceId id = broadcast.id();
    f.project.addSequence(std::move(broadcast));

    const auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    // The file must not contain a rounded rate anywhere.
    CHECK(text->find("30000/1001") != std::string::npos);
    CHECK(text->find("29.97") == std::string::npos);

    const auto loaded = io::loadProjectFromString(*text);
    REQUIRE(loaded);
    CHECK(loaded->project.findSequence(id)->frameRate() == time::rates::fps29_97);
}

TEST_CASE("Ids are stable across save and load", "[io]") {
    Fixture f = populated();
    const model::ClipId firstClip = f.track(f.v1).clips()[0].id;

    const auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    auto loaded = io::loadProjectFromString(*text);
    REQUIRE(loaded);

    const model::Sequence* sequence = loaded->project.findSequence(f.sequenceId);
    REQUIRE(sequence != nullptr);
    REQUIRE(sequence->findTrack(f.v1) != nullptr);
    CHECK(sequence->findTrack(f.v1)->clips()[0].id == firstClip);

    SECTION("and new ids do not collide with loaded ones") {
        const auto fresh = loaded->project.ids().next<model::ClipTag>();
        CHECK(fresh.value() > firstClip.value());
        for (const model::Sequence& s : loaded->project.sequences()) {
            for (const model::Track& t : s.videoTracks()) {
                for (const model::Clip& c : t.clips()) {
                    CHECK(c.id != fresh);
                }
            }
        }
    }
}

TEST_CASE("Track flags and sequence settings round trip", "[io]") {
    Fixture f = populated();
    const auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    const auto loaded = io::loadProjectFromString(*text);
    REQUIRE(loaded);

    const model::Sequence* sequence = loaded->project.findSequence(f.sequenceId);
    REQUIRE(sequence != nullptr);
    CHECK(sequence->findTrack(f.v2)->isMuted());
    CHECK(sequence->findTrack(f.a1)->isLocked());
    CHECK(sequence->startTime().frames() == 107892);
    CHECK(sequence->width() == 1920);
    CHECK(sequence->audioSampleRate() == time::rates::hz48000);
}

TEST_CASE("A clip's own filtering and compression survive a save", "[io]") {
    // The track has a pair of these too, and they are written by the same
    // encoder -- so this also guards the track's, which had its own copy of
    // the same six fields until the clip needed them.
    Fixture f = populated();
    const auto trackId = f.a1;
    // `populated` locks A1, and a locked track refuses edits -- rightly, and
    // it is what the lock is for.
    f.track(trackId).setLocked(false);
    const auto clipId = f.track(trackId).clips().front().id;

    model::AudioEq eq;
    eq.enabled = true;
    eq.highPassHz = 80.0;
    eq.peakHz = 2500.0;
    eq.peakGainDb = -4.5;
    eq.peakQ = 2.25;
    model::Compressor compressor;
    compressor.enabled = true;
    compressor.thresholdDb = -21.5;
    compressor.ratio = 4.0;
    compressor.attackMs = 3.0;
    compressor.releaseMs = 180.0;
    compressor.makeupDb = 2.5;

    edit::CommandStack stack;
    auto built = edit::makeSetClipProcessing(f.project, {f.sequenceId, trackId}, clipId, eq,
                                             compressor);
    REQUIRE(built);
    stack.execute(f.project, std::move(*built));

    const auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    const auto loaded = io::loadProjectFromString(*text);
    REQUIRE(loaded);

    const model::Clip* back =
        loaded->project.findSequence(f.sequenceId)->findTrack(trackId)->find(clipId);
    REQUIRE(back != nullptr);
    CHECK(back->eq == eq);
    CHECK(back->compressor == compressor);

    // And a clip that was never processed carries nothing, so a project made
    // before any of this existed reads back exactly as it did.
    const model::Clip& untouched =
        loaded->project.findSequence(f.sequenceId)->findTrack(f.v1)->clips().front();
    CHECK_FALSE(untouched.eq.enabled);
    CHECK_FALSE(untouched.compressor.enabled);
}

TEST_CASE("Fields written by a newer build are not destroyed", "[io]") {
    // The scenario: someone opens a project in an older build and saves it.
    // Everything the newer build added has to still be there afterwards.
    Fixture f = populated();
    const auto text = io::saveProjectToString(f.project);
    REQUIRE(text);

    std::string enriched = *text;
    REQUIRE(enriched.find("\"activeSequence\":") != std::string::npos);
    // The document's own opening brace, not some nested one: this field
    // belongs at the top level, which is where an older build would meet it
    // and have to leave it alone.
    REQUIRE(enriched.starts_with("{\n"));
    enriched.insert(2, "  \"effectsFromTheFuture\": [\"lumetri\", \"warp\"],\n");

    const auto loaded = io::loadProjectFromString(enriched);
    REQUIRE(loaded);
    const auto resaved = io::saveProjectToString(loaded->project, loaded->unknown);
    REQUIRE(resaved);

    INFO("resaved:\n" << *resaved);
    CHECK(resaved->find("effectsFromTheFuture") != std::string::npos);
    CHECK(resaved->find("lumetri") != std::string::npos);

    SECTION("and dropping the carrier does lose them, which is why it exists") {
        const auto without = io::saveProjectToString(loaded->project);
        REQUIRE(without);
        CHECK(without->find("effectsFromTheFuture") == std::string::npos);
    }
}

TEST_CASE("Unknown fields on a clip follow the clip", "[io]") {
    Fixture f = populated();
    const auto text = io::saveProjectToString(f.project);
    REQUIRE(text);

    // Attach something to the clip this test is about -- the first on V1 --
    // and then reorder the timeline so that clip is no longer at the same
    // array index.
    //
    // Anchored on that clip's own id rather than on the first "sourceRange" in
    // the document, which is not the same thing: "audioTracks" sorts before
    // "videoTracks", so the first one belongs to the audio clip, and this test
    // used to enrich a clip it never moved and then check that the field
    // survived a move it was not part of.
    const model::ClipId moving = f.track(f.v1).clips()[0].id;
    const std::string anchor = "\"id\": " + std::to_string(moving.value()) + ",";
    std::string enriched = *text;
    const auto clipAt = enriched.find(anchor);
    REQUIRE(clipAt != std::string::npos);
    // Ids come from one counter shared by every kind of object, so this one
    // appears exactly once and cannot enrich something else by accident.
    REQUIRE(enriched.find(anchor, clipAt + anchor.size()) == std::string::npos);
    enriched.insert(clipAt, "\"speed\": \"1/2\",\n          ");

    auto loaded = io::loadProjectFromString(enriched);
    REQUIRE(loaded);

    edit::CommandStack stack;
    const model::ClipId first =
        loaded->project.findSequence(f.sequenceId)->findTrack(f.v1)->clips()[0].id;
    auto moved = edit::makeMove(loaded->project, {f.sequenceId, f.v1}, first, f.v1,
                                time::RationalTime{500, time::rates::fps25});
    REQUIRE(moved);
    stack.execute(loaded->project, std::move(*moved));

    const auto resaved = io::saveProjectToString(loaded->project, loaded->unknown);
    REQUIRE(resaved);
    // Matched by id rather than by position, so the reorder does not lose it.
    // Printed only on failure, and worth the noise: which clip was enriched
    // and what came back are the two things anyone diagnosing this from a CI
    // log has to guess at otherwise.
    INFO("enriched the clip with id " << moving.value() << "; resaved:\n" << *resaved);
    CHECK(resaved->find("\"speed\"") != std::string::npos);
}

TEST_CASE("Malformed input is rejected with something readable", "[io]") {
    SECTION("not JSON at all") {
        const auto loaded = io::loadProjectFromString("this is not json");
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().message().find("valid JSON") != std::string::npos);
    }

    SECTION("JSON, but not a project") {
        const auto loaded = io::loadProjectFromString(R"({"something": "else"})");
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().message().find("CutReel project") != std::string::npos);
    }

    SECTION("a project with no schema version") {
        const auto loaded = io::loadProjectFromString(R"({"zaro": {}})");
        REQUIRE_FALSE(loaded);
    }

    SECTION("a clip with no id") {
        const auto loaded = io::loadProjectFromString(R"({
            "zaro": {"schemaVersion": 1},
            "sequences": [{"id": 1, "frameRate": "25", "videoTracks": [
                {"id": 2, "clips": [{"sourceRange": {"start": {"frames": 0, "rate": "25"},
                                                     "duration": {"frames": 10, "rate": "25"}},
                                     "timelineRange": {"start": {"frames": 0, "rate": "25"},
                                                       "duration": {"frames": 10, "rate": "25"}}}]}
            ]}]})");
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().message().find("no id") != std::string::npos);
    }

    SECTION("a rate that is not a rate") {
        const auto loaded = io::loadProjectFromString(R"({
            "zaro": {"schemaVersion": 1},
            "sequences": [{"id": 1, "frameRate": "sometimes"}]})");
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().message().find("as a rate") != std::string::npos);
    }
}

TEST_CASE("Writing and reading a file", "[io]") {
    Fixture f = populated();
    const std::string path = std::string{ZARO_SCRATCH_DIR} + "/roundtrip.zaro";

    REQUIRE(io::saveProject(f.project, path).ok());
    const auto loaded = io::loadProject(path);
    REQUIRE(loaded);
    CHECK(loaded->project == f.project);

    SECTION("a missing file reports not found") {
        const auto missing = io::loadProject(path + ".nope");
        REQUIRE_FALSE(missing);
        CHECK(missing.error().code() == ErrorCode::NotFound);
    }
}

TEST_CASE("Every serializable field survives, set to a non-default value", "[io][exhaustive]") {
    // Two encoder gaps have been found by accident so far: audio stream info,
    // and track gain and pan. Both were invisible to the existing round-trip
    // tests, because those compare models and a field the encoder skips will
    // match whatever the decoder defaults it to.
    //
    // So this sets every field to something distinctive -- never a default --
    // and checks each one individually after a round trip. A field the encoder
    // forgets now fails here rather than in someone's project.
    model::Project project;

    model::MediaRef ref;
    ref.id = project.ids().next<model::MediaRefTag>();
    ref.path = "/media/take-01.mov";
    ref.contentHash = "deadbeefcafe0001";
    ref.proxyPath = "/media/proxies/take-01_proxy.mov";
    ref.name = "take-01.mov";
    ref.info.duration = time::Rational{1001 * 240, 30000};
    {
        media::VideoStreamInfo video;
        video.width = 4096;
        video.height = 2160;
        video.frameRate = time::rates::fps29_97;
        ref.info.videoStreams.push_back(video);

        media::AudioStreamInfo audio;
        audio.sampleRate = time::rates::hz96000;
        audio.channelCount = 6;
        ref.info.audioStreams.push_back(audio);
    }
    const model::MediaRefId mediaId = project.addMedia(ref);
    project.setUsingProxies(true);

    model::Sequence sequence{project.ids().next<model::SequenceTag>(), "Reel 3 — final",
                             time::rates::fps23_976};
    const model::SequenceId sequenceId = sequence.id();
    sequence.setAudioSampleRate(time::rates::hz96000);
    sequence.setSize(4096, 1716);
    sequence.setStartTime(time::RationalTime{86400, time::rates::fps23_976});

    {
        model::CaptionStyle style;
        style.family = "Avenir Next";
        style.pointSize = 54.5;
        style.bold = true;
        style.bottomMargin = 112.25;
        style.widthFraction = 0.675;
        style.red = 0.95;
        style.green = 0.9;
        style.blue = 0.15;
        style.alpha = 0.85;
        sequence.captions().setStyle(style);
        sequence.captions().setBurnedIn(true);

        model::Caption first;
        first.range =
            time::TimeRange::fromStartEnd(time::RationalTime{1500, time::Rational{1000, 1}},
                                          time::RationalTime{4250, time::Rational{1000, 1}});
        first.text = "First caption\nsecond line";
        sequence.captions().add(first);

        model::Caption second;
        second.range =
            time::TimeRange::fromStartEnd(time::RationalTime{9000, time::Rational{1000, 1}},
                                          time::RationalTime{11000, time::Rational{1000, 1}});
        second.text = "Later";
        sequence.captions().add(second);
    }

    const auto videoTrackId = project.ids().next<model::TrackTag>();
    const auto audioTrackId = project.ids().next<model::TrackTag>();
    sequence.addTrack(videoTrackId, model::TrackKind::Video, "V-main");
    sequence.addTrack(audioTrackId, model::TrackKind::Audio, "A-dialogue");

    model::Track& video = sequence.tracksMutable(model::TrackKind::Video).front();
    video.setMuted(true);
    video.setSoloed(true);
    video.setLocked(true);
    video.setGainDb(-2.25);
    video.setPan(-0.4);

    model::Track& audio = sequence.tracksMutable(model::TrackKind::Audio).front();
    {
        model::AudioEq eq;
        eq.enabled = true;
        eq.highPassHz = 82.5;
        eq.lowPassHz = 14250.0;
        eq.peakHz = 2350.0;
        eq.peakGainDb = -4.25;
        eq.peakQ = 1.75;
        audio.setEq(eq);

        model::Compressor compressor;
        compressor.enabled = true;
        compressor.thresholdDb = -14.5;
        compressor.ratio = 3.75;
        compressor.attackMs = 7.25;
        compressor.releaseMs = 185.0;
        compressor.makeupDb = 2.25;
        audio.setCompressor(compressor);
    }
    audio.setGainDb(7.5);
    audio.setPan(0.9);

    const auto makeClip = [&](std::int64_t start, std::int64_t length, std::int64_t sourceStart) {
        model::Clip clip;
        clip.id = project.ids().next<model::ClipTag>();
        clip.source = mediaId;
        clip.name = "clip " + std::to_string(start);
        clip.enabled = false;
        clip.sourceRange = time::TimeRange{time::RationalTime{sourceStart, time::rates::fps23_976},
                                           time::RationalTime{length, time::rates::fps23_976}};
        clip.timelineRange = time::TimeRange{time::RationalTime{start, time::rates::fps23_976},
                                             time::RationalTime{length, time::rates::fps23_976}};
        clip.transform.positionX = -12.5;
        clip.transform.positionY = 33.25;
        clip.transform.scaleX = 1.75;
        clip.transform.scaleY = 0.625;
        clip.transform.rotationDegrees = -47.5;
        clip.transform.anchorX = 8.125;
        clip.transform.anchorY = -4.75;
        clip.transform.opacity = 0.375;
        clip.blend = model::BlendMode::Multiply;
        clip.gainDb = -11.5;
        clip.pan = 0.6;
        clip.color.temperature = -22.5;
        clip.color.tint = 13.25;
        clip.color.exposure = 1.75;
        clip.color.contrast = -37.5;
        clip.color.saturation = 143.75;
        clip.curves.master.set(model::CurvePoint{0.0, 0.05});
        clip.curves.master.set(model::CurvePoint{0.5, 0.62});
        clip.curves.master.set(model::CurvePoint{1.0, 0.95});
        clip.curves.red.set(model::CurvePoint{0.25, 0.3});
        clip.curves.red.set(model::CurvePoint{0.75, 0.8});
        clip.curves.blue.set(model::CurvePoint{0.1, 0.0});
        clip.curves.blue.set(model::CurvePoint{0.9, 1.0});
        {
            model::Clip::Angle first;
            first.media = mediaId;
            first.offset = time::RationalTime{0, time::rates::fps23_976};
            first.name = "A camera";
            model::Clip::Angle second;
            second.media = mediaId;
            second.offset = time::RationalTime{137, time::rates::fps23_976};
            second.name = "B camera";
            clip.angles = {first, second};
            clip.activeAngle = 1;
        }
        clip.adjustment = true;
        clip.reversed = true;
        clip.mask.shape = model::MaskShape::Ellipse;
        clip.mask.width = 640.5;
        clip.mask.height = 360.25;
        clip.mask.centreX = -32.125;
        clip.mask.centreY = 48.75;
        clip.mask.cornerRadius = 22.5;
        clip.mask.feather = 14.25;
        clip.mask.inverted = true;
        clip.graphic.kind = model::GraphicKind::Ellipse;
        clip.graphic.width = 512.5;
        clip.graphic.height = 288.25;
        clip.graphic.centreX = -64.75;
        clip.graphic.centreY = 32.125;
        clip.graphic.cornerRadius = 18.5;
        clip.graphic.feather = 6.25;
        clip.graphic.red = 0.125;
        clip.graphic.green = 0.875;
        clip.graphic.blue = 0.375;
        clip.graphic.alpha = 0.625;
        clip.graphic.text = "Chapter Two";
        clip.graphic.family = "Helvetica Neue";
        clip.graphic.pointSize = 96.5;
        clip.graphic.bold = true;
        clip.graphic.italic = true;
        clip.graphic.alignment = -1;
        clip.lut.path = "/looks/kodak-2383.cube";
        clip.lut.amount = 0.625;
        clip.secondary.qualifier.enabled = true;
        clip.secondary.qualifier.hueCentre = 214.5;
        clip.secondary.qualifier.hueWidth = 47.25;
        clip.secondary.qualifier.hueSoftness = 12.75;
        clip.secondary.qualifier.saturationLow = 0.185;
        clip.secondary.qualifier.saturationHigh = 0.925;
        clip.secondary.qualifier.saturationSoftness = 0.075;
        clip.secondary.qualifier.lumaLow = 0.135;
        clip.secondary.qualifier.lumaHigh = 0.865;
        clip.secondary.qualifier.lumaSoftness = 0.115;
        clip.secondary.correction.temperature = 31.5;
        clip.secondary.correction.exposure = -0.875;
        clip.secondary.correction.saturation = 62.5;
        // One curve of each interpolation, with handles that are not the
        // default ease, so a lost handle or a mode collapsing to linear shows
        // up here rather than as a fade with the wrong shape.
        {
            model::Keyframe held;
            held.time = time::RationalTime{3, time::rates::fps23_976};
            held.value = 0.125;
            held.interpolation = model::Interpolation::Hold;
            clip.animation.curve(model::Param::Opacity).set(held);

            model::Keyframe eased;
            eased.time = time::RationalTime{17, time::rates::fps23_976};
            eased.value = 0.875;
            eased.interpolation = model::Interpolation::Bezier;
            eased.out = model::Handle{0.4, 0.05};
            eased.in = model::Handle{0.15, -0.02};
            clip.animation.curve(model::Param::Opacity).set(eased);

            model::Keyframe swept;
            swept.time = time::RationalTime{9, time::rates::fps23_976};
            swept.value = -320.5;
            clip.animation.curve(model::Param::PositionX).set(swept);

            model::Keyframe ridden;
            ridden.time = time::RationalTime{4, time::rates::fps23_976};
            ridden.value = -18.25;
            ridden.interpolation = model::Interpolation::Bezier;
            clip.animation.curve(model::Param::GainDb).set(ridden);
        }
        return clip;
    };

    const model::Clip first = makeClip(0, 48, 100);
    const model::Clip second = makeClip(48, 48, 400);
    video.insert(first);
    video.insert(second);

    model::Transition dissolve;
    dissolve.id = project.ids().next<model::TransitionTag>();
    dissolve.from = first.id;
    dissolve.to = second.id;
    dissolve.range = time::TimeRange{time::RationalTime{40, time::rates::fps23_976},
                                     time::RationalTime{16, time::rates::fps23_976}};
    dissolve.kind = model::TransitionKind::CrossDissolve;
    video.setTransitions({dissolve});

    project.addSequence(std::move(sequence));
    project.setActiveSequence(sequenceId);

    // --- Round trip ---------------------------------------------------------
    const auto text = io::saveProjectToString(project);
    REQUIRE(text);
    const auto loaded = io::loadProjectFromString(*text);
    REQUIRE(loaded);
    const model::Project& back = loaded->project;

    CHECK(back.activeSequence() == sequenceId);

    // Media, including the cached stream info that operator== deliberately
    // ignores -- which is exactly why it needs checking by hand.
    const model::MediaRef* loadedRef = back.findMedia(mediaId);
    REQUIRE(loadedRef != nullptr);
    CHECK(loadedRef->path == ref.path);
    CHECK(loadedRef->contentHash == ref.contentHash);
    CHECK(loadedRef->name == ref.name);
    CHECK(loadedRef->info.duration == ref.info.duration);
    REQUIRE(loadedRef->info.primaryVideo() != nullptr);
    CHECK(loadedRef->info.primaryVideo()->width == 4096);
    CHECK(loadedRef->info.primaryVideo()->height == 2160);
    CHECK(loadedRef->info.primaryVideo()->frameRate == time::rates::fps29_97);
    REQUIRE(loadedRef->info.primaryAudio() != nullptr);
    CHECK(loadedRef->info.primaryAudio()->sampleRate == time::rates::hz96000);
    CHECK(loadedRef->info.primaryAudio()->channelCount == 6);

    // Sequence.
    const model::Sequence* loadedSequence = back.findSequence(sequenceId);
    REQUIRE(loadedSequence != nullptr);
    CHECK(loadedSequence->name() == "Reel 3 — final");
    CHECK(loadedSequence->frameRate() == time::rates::fps23_976);
    CHECK(loadedSequence->audioSampleRate() == time::rates::hz96000);
    CHECK(loadedSequence->width() == 4096);
    CHECK(loadedSequence->height() == 1716);
    CHECK(loadedSequence->startTime().frames() == 86400);

    // Tracks, both kinds, every flag.
    const model::Track* loadedVideo = loadedSequence->findTrack(videoTrackId);
    REQUIRE(loadedVideo != nullptr);
    CHECK(loadedVideo->name() == "V-main");
    CHECK(loadedVideo->kind() == model::TrackKind::Video);
    CHECK(loadedVideo->isMuted());
    CHECK(loadedVideo->isLocked());
    // Against the project's own sequence: the local was moved into the project
    // long before this point, and comparing with a moved-from object compares
    // nothing.
    CHECK(loadedSequence->captions() == project.findSequence(sequenceId)->captions());
    REQUIRE(back.media().size() == 1);
    CHECK(back.media().front().proxyPath == "/media/proxies/take-01_proxy.mov");
    CHECK(back.usingProxies());

    CHECK(loadedSequence->captions().isBurnedIn());
    REQUIRE(loadedSequence->captions().size() == 2);
    CHECK(loadedSequence->captions().captions()[0].text == "First caption\nsecond line");

    CHECK(loadedVideo->isSoloed());
    CHECK(loadedVideo->gainDb() == -2.25);
    CHECK(loadedVideo->pan() == -0.4);

    const model::Track* loadedAudio = loadedSequence->findTrack(audioTrackId);
    REQUIRE(loadedAudio != nullptr);
    CHECK(loadedAudio->kind() == model::TrackKind::Audio);
    CHECK(loadedAudio->eq() == project.findSequence(sequenceId)->audioTracks().front().eq());
    CHECK(loadedAudio->compressor() ==
          project.findSequence(sequenceId)->audioTracks().front().compressor());
    CHECK(loadedAudio->gainDb() == 7.5);
    CHECK(loadedAudio->pan() == 0.9);

    // Clip, every field.
    const model::Clip* loadedClip = loadedVideo->find(first.id);
    REQUIRE(loadedClip != nullptr);
    CHECK(loadedClip->source == mediaId);
    CHECK(loadedClip->name == first.name);
    CHECK_FALSE(loadedClip->enabled);
    CHECK(loadedClip->sourceRange == first.sourceRange);
    CHECK(loadedClip->timelineRange == first.timelineRange);
    CHECK(loadedClip->transform == first.transform);
    CHECK(loadedClip->blend == model::BlendMode::Multiply);
    CHECK(loadedClip->gainDb == -11.5);
    CHECK(loadedClip->pan == 0.6);
    CHECK(loadedClip->animation == first.animation);
    CHECK(loadedClip->color == first.color);
    CHECK(loadedClip->curves == first.curves);
    CHECK(loadedClip->secondary == first.secondary);
    CHECK(loadedClip->lut == first.lut);
    CHECK(loadedClip->graphic == first.graphic);
    CHECK(loadedClip->mask == first.mask);
    CHECK(loadedClip->reversed);
    CHECK(loadedClip->angles == first.angles);
    CHECK(loadedClip->activeAngle == 1);
    CHECK(loadedClip->adjustment);

    // Transition, every field.
    REQUIRE(loadedVideo->transitions().size() == 1);
    const model::Transition& loadedTransition = loadedVideo->transitions().front();
    CHECK(loadedTransition.id == dissolve.id);
    CHECK(loadedTransition.from == first.id);
    CHECK(loadedTransition.to == second.id);
    CHECK(loadedTransition.range == dissolve.range);
    CHECK(loadedTransition.kind == model::TransitionKind::CrossDissolve);

    // And the whole thing compares equal, which the field checks above make
    // meaningful rather than merely reassuring.
    CHECK(back == project);
}

TEST_CASE("Ids issued after loading never collide with ids already in the file",
          "[io][exhaustive]") {
    // Every id type shares one counter, so the loader has to see all of them.
    // Missing one restarts the counter below an id already in use, and the next
    // thing created silently collides -- worse than a field failing to round
    // trip, because the file is fine and the damage happens later, in memory,
    // to whoever opens it.
    //
    // Markers, transitions and links were each added after this was written,
    // and the loader was not updated for any of them.
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(50, 50, 600))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 100, 500))));

    const model::ClipId video = f.track(f.v1).clips()[0].id;
    const model::ClipId audio = f.track(f.a1).clips()[0].id;
    REQUIRE(f.run(edit::makeLinkClips(f.project, f.sequenceId, {{f.v1, video}, {f.a1, audio}})));
    REQUIRE(f.run(edit::makeAddCrossDissolve(f.project, f.on(f.v1), f.at(50), f.at(10))));
    REQUIRE(f.run(edit::makeAddMarker(f.project, f.sequenceId, f.at(20), f.at(0), "Here")));

    // Collect every id the file contains.
    std::set<std::uint64_t> used;
    const auto collect = [&used](const model::Project& project) {
        used.clear();
        for (const model::MediaRef& ref : project.media()) {
            used.insert(ref.id.value());
        }
        for (const model::Sequence& sequence : project.sequences()) {
            used.insert(sequence.id().value());
            for (const model::Marker& marker : sequence.markers()) {
                used.insert(marker.id.value());
            }
            for (const auto* list : {&sequence.videoTracks(), &sequence.audioTracks()}) {
                for (const model::Track& track : *list) {
                    used.insert(track.id().value());
                    for (const model::Clip& clip : track.clips()) {
                        used.insert(clip.id.value());
                        if (clip.link.isValid()) {
                            used.insert(clip.link.value());
                        }
                    }
                    for (const model::Transition& transition : track.transitions()) {
                        used.insert(transition.id.value());
                    }
                }
            }
        }
    };

    const auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    auto loaded = io::loadProjectFromString(*text);
    REQUIRE(loaded);
    collect(loaded->project);
    REQUIRE(used.size() > 6);  // media, sequence, tracks, clips, link, transition, marker

    // Everything issued from here on has to be new, whatever kind it is.
    for (int i = 0; i < 50; ++i) {
        const std::uint64_t next = loaded->project.ids().next<model::ClipTag>().value();
        INFO("issued " << next);
        CHECK(used.find(next) == used.end());
    }
}
