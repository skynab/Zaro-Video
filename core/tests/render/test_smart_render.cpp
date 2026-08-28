#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/SmartRender.h"

using namespace zaro;

namespace {

const time::Rational k25 = time::rates::fps25;

time::RationalTime at(std::int64_t frames) {
    return time::RationalTime{frames, k25};
}

/// A project holding one untouched clip of one file: the case a copy is for.
struct Straight {
    model::Project project;
    model::SequenceId sequenceId;
    model::TrackId trackId;
    model::ClipId clipId;
    model::MediaRefId mediaId;

    Straight() {
        model::MediaRef ref;
        ref.id = project.ids().next<model::MediaRefTag>();
        ref.path = "/rushes/shot.mov";
        ref.name = "shot";
        media::VideoStreamInfo video;
        video.codecName = "prores";
        video.width = 1920;
        video.height = 1080;
        video.frameRate = k25;
        ref.info.videoStreams.push_back(video);
        media::AudioStreamInfo audio;
        audio.codecName = "pcm_s16le";
        audio.sampleRate = time::rates::hz48000;
        audio.channelCount = 2;
        ref.info.audioStreams.push_back(audio);
        ref.info.duration = time::Rational{40, 1};
        mediaId = project.addMedia(ref);

        model::Sequence sequence{project.ids().next<model::SequenceTag>(), "Sequence", k25};
        sequence.setSize(1920, 1080);
        sequenceId = sequence.id();
        trackId = project.ids().next<model::TrackTag>();
        sequence.addTrack(trackId, model::TrackKind::Video, "V1");

        model::Clip clip;
        clip.id = project.ids().next<model::ClipTag>();
        clip.source = mediaId;
        clip.timelineRange = time::TimeRange{at(0), at(100)};
        clip.sourceRange = time::TimeRange{at(25), at(100)};
        clipId = clip.id;
        sequence.findTrack(trackId)->insert(std::move(clip));
        project.addSequence(std::move(sequence));
    }

    [[nodiscard]] model::Sequence& sequence() { return *project.findSequence(sequenceId); }
    [[nodiscard]] model::Clip& clip() { return *sequence().findTrack(trackId)->find(clipId); }

    [[nodiscard]] render::SmartRenderTarget target() const {
        render::SmartRenderTarget wanted;
        wanted.width = 1920;
        wanted.height = 1080;
        wanted.frameRate = k25;
        wanted.videoCodec = "prores";
        wanted.includeAudio = true;
        wanted.audioSampleRate = time::rates::hz48000;
        wanted.audioChannels = 2;
        return wanted;
    }

    [[nodiscard]] render::SmartRenderPlan plan() {
        return render::smartRenderPlan(project, sequence(), 0, 100, target());
    }
};

}  // namespace

TEST_CASE("an untouched clip at matching settings can be copied", "[smartrender]") {
    Straight world;
    const render::SmartRenderPlan plan = world.plan();
    CHECK(plan.possible);
    CHECK(plan.copyAudio);
    CHECK(plan.media == world.mediaId);
    // Where in the *file* the export starts, not where on the timeline.
    CHECK(plan.sourceStart == at(25));
    CHECK(plan.frames == 100);
}

TEST_CASE("anything done to the picture stops a copy, and says what", "[smartrender]") {
    SECTION("a grade") {
        Straight world;
        world.clip().color.exposure = 0.5;
        const auto plan = world.plan();
        CHECK_FALSE(plan.possible);
        CHECK(plan.reason.find("graded") != std::string::npos);
    }
    SECTION("a transform") {
        Straight world;
        world.clip().transform.scaleX = 1.2;
        CHECK_FALSE(world.plan().possible);
    }
    SECTION("a fade") {
        Straight world;
        world.clip().transform.opacity = 0.5;
        CHECK_FALSE(world.plan().possible);
    }
    SECTION("a keyframe") {
        Straight world;
        world.clip()
            .animation.curve(model::Param::Opacity)
            .set(model::Keyframe{at(0), 1.0, model::Interpolation::Linear, {}, {}});
        CHECK_FALSE(world.plan().possible);
    }
    SECTION("a mask") {
        Straight world;
        world.clip().mask.shape = model::MaskShape::Ellipse;
        CHECK_FALSE(world.plan().possible);
    }
    SECTION("an effect") {
        Straight world;
        model::Effect blur;
        blur.kind = model::EffectKind::Blur;
        blur.setValue(model::EffectParam::Radius, 4.0);
        world.clip().effects.push_back(blur);
        CHECK_FALSE(world.plan().possible);
    }
    SECTION("a retime") {
        Straight world;
        world.clip().timelineRange = time::TimeRange{at(0), at(50)};
        CHECK_FALSE(world.plan().possible);
    }
    SECTION("being reversed") {
        Straight world;
        world.clip().reversed = true;
        CHECK_FALSE(world.plan().possible);
    }
}

TEST_CASE("an effect that is at its defaults does not stop a copy", "[smartrender]") {
    Straight world;
    // Added and not touched: it changes nothing, and refusing here would mean
    // an export somebody cannot explain.
    world.clip().effects.emplace_back();
    CHECK(world.plan().possible);
}

TEST_CASE("the export settings have to match the file", "[smartrender]") {
    SECTION("a different codec") {
        Straight world;
        auto wanted = world.target();
        wanted.videoCodec = "libx264";
        const auto plan = render::smartRenderPlan(world.project, world.sequence(), 0, 100, wanted);
        CHECK_FALSE(plan.possible);
        CHECK(plan.reason.find("codec") != std::string::npos);
    }
    SECTION("a different size") {
        Straight world;
        world.sequence().setSize(1280, 720);
        auto wanted = world.target();
        wanted.width = 1280;
        wanted.height = 720;
        CHECK_FALSE(
            render::smartRenderPlan(world.project, world.sequence(), 0, 100, wanted).possible);
    }
    SECTION("audio that would have to be re-encoded") {
        Straight world;
        auto wanted = world.target();
        wanted.audioChannels = 6;
        const auto plan = render::smartRenderPlan(world.project, world.sequence(), 0, 100, wanted);
        CHECK_FALSE(plan.possible);
        CHECK(plan.reason.find("audio") != std::string::npos);
    }
    SECTION("no audio wanted at all is still a copy") {
        Straight world;
        auto wanted = world.target();
        wanted.includeAudio = false;
        const auto plan = render::smartRenderPlan(world.project, world.sequence(), 0, 100, wanted);
        CHECK(plan.possible);
        CHECK_FALSE(plan.copyAudio);
    }
}

TEST_CASE("anything else on screen stops a copy", "[smartrender]") {
    Straight world;
    const auto second = world.project.ids().next<model::TrackTag>();
    world.sequence().addTrack(second, model::TrackKind::Video, "V2");
    model::Clip title;
    title.id = world.project.ids().next<model::ClipTag>();
    title.graphic.kind = model::GraphicKind::Text;
    title.graphic.text = "over it";
    title.timelineRange = time::TimeRange{at(80), at(20)};
    title.sourceRange = time::TimeRange{at(0), at(20)};
    world.sequence().findTrack(second)->insert(std::move(title));

    const auto plan = world.plan();
    CHECK_FALSE(plan.possible);
    CHECK(plan.reason.find("more than one clip") != std::string::npos);
}

TEST_CASE("a range the clip does not cover stops a copy", "[smartrender]") {
    Straight world;
    const auto plan =
        render::smartRenderPlan(world.project, world.sequence(), 0, 200, world.target());
    CHECK_FALSE(plan.possible);
    CHECK(plan.reason.find("cover") != std::string::npos);
}

TEST_CASE("being on proxies stops a copy", "[smartrender]") {
    Straight world;
    for (model::MediaRef& media : world.project.mediaMutable()) {
        media.proxyPath = "/rushes/shot-proxy.mov";
    }
    world.project.setUsingProxies(true);
    const auto plan = world.plan();
    CHECK_FALSE(plan.possible);
    // The one mistake that would ship at the wrong quality unnoticed.
    CHECK(plan.reason.find("proxies") != std::string::npos);
}
