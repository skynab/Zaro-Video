#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"

#include "Fixtures.h"

using namespace zaro;
using zaro::testing::fixture;

namespace {

/// A one-clip project over a real fixture, so the render has something to do.
struct Renderable {
    model::Project project;
    model::SequenceId sequence;
    bool ok{false};
};

Renderable buildProject(const std::string& file) {
    Renderable out;
    auto probed = platform::ffmpeg::probe(fixture(file));
    if (!probed) {
        return out;
    }

    model::MediaRef ref;
    ref.id = out.project.ids().next<model::MediaRefTag>();
    ref.path = fixture(file);
    ref.name = file;
    ref.info = *probed;
    const auto mediaId = out.project.addMedia(ref);

    const media::VideoStreamInfo* video = probed->primaryVideo();
    const time::Rational rate = video != nullptr ? video->frameRate : time::rates::fps25;

    model::Sequence sequence{out.project.ids().next<model::SequenceTag>(), "Test", rate};
    sequence.setSize(video != nullptr ? video->width : 320, video != nullptr ? video->height : 240);
    out.sequence = sequence.id();
    const auto trackId = out.project.ids().next<model::TrackTag>();
    sequence.addTrack(trackId, model::TrackKind::Video, "V1");

    model::Clip clip;
    clip.id = out.project.ids().next<model::ClipTag>();
    clip.source = mediaId;
    clip.name = file;
    const auto length = time::RationalTime::fromSeconds(probed->duration, rate);
    clip.sourceRange = time::TimeRange{time::RationalTime{0, rate}, length};
    clip.timelineRange = time::TimeRange{time::RationalTime{0, rate}, length};
    sequence.tracksMutable(model::TrackKind::Video).front().insert(clip);

    out.project.addSequence(std::move(sequence));
    out.ok = true;
    return out;
}

std::string outputPath(const char* name) {
    return (std::filesystem::path{ZARO_SCRATCH_DIR} / name).string();
}

}  // namespace

TEST_CASE("renderSequence writes a complete file", "[render][export]") {
    ZARO_REQUIRE_FIXTURE("ladder_prores.mov");
    Renderable renderable = buildProject("ladder_prores.mov");
    REQUIRE(renderable.ok);

    platform::ffmpeg::RenderRequest request;
    request.outputPath = outputPath("render-test.mov");
    request.sequence = renderable.sequence;
    request.frameCount = 40;
    request.includeAudio = false;

    std::int64_t lastSeen = 0;
    platform::ffmpeg::RenderSummary summary;
    const auto status = platform::ffmpeg::renderSequence(
        renderable.project, request, [&](const platform::ffmpeg::RenderProgress& progress) {
            // Progress only ever moves forward, and reports a total.
            CHECK(progress.framesDone > lastSeen);
            CHECK(progress.framesTotal == 40);
            lastSeen = progress.framesDone;
        });
    REQUIRE(status.ok());
    CHECK(lastSeen == 40);

    // Every frame sent to the encoder reached the file. The gap between these
    // two is what a silently truncated export looks like.
    REQUIRE(platform::ffmpeg::renderSequence(renderable.project, request, {}, {}, &summary).ok());
    CHECK(summary.framesEncoded == 40);
    CHECK(summary.videoPacketsWritten == summary.framesEncoded);

    CHECK(std::filesystem::exists(request.outputPath));
    const auto probed = platform::ffmpeg::probe(request.outputPath);
    REQUIRE(probed);
    REQUIRE(probed->primaryVideo() != nullptr);
    CHECK(probed->primaryVideo()->width == 320);
}

TEST_CASE("A render can be abandoned part way", "[render][export]") {
    // Without this, quitting during an export means waiting for the whole thing.
    ZARO_REQUIRE_FIXTURE("ladder_prores.mov");
    Renderable renderable = buildProject("ladder_prores.mov");
    REQUIRE(renderable.ok);

    platform::ffmpeg::RenderRequest request;
    request.outputPath = outputPath("render-cancelled.mov");
    request.sequence = renderable.sequence;
    request.frameCount = 300;
    request.includeAudio = false;

    std::int64_t rendered = 0;
    const auto status = platform::ffmpeg::renderSequence(
        renderable.project, request,
        [&](const platform::ffmpeg::RenderProgress& progress) { rendered = progress.framesDone; },
        [&] { return rendered < 10; });

    REQUIRE_FALSE(status.ok());
    CHECK(status.error().code() == ErrorCode::Cancelled);
    // It stopped early rather than running to the end and reporting after.
    CHECK(rendered < 40);
}

TEST_CASE("Rendering a range starts where it is told", "[render][export]") {
    ZARO_REQUIRE_FIXTURE("ladder_prores.mov");
    Renderable renderable = buildProject("ladder_prores.mov");
    REQUIRE(renderable.ok);

    platform::ffmpeg::RenderRequest request;
    request.outputPath = outputPath("render-range.mov");
    request.sequence = renderable.sequence;
    request.startFrame = 100;
    request.frameCount = 20;
    request.includeAudio = false;

    platform::ffmpeg::RenderSummary summary;
    REQUIRE(platform::ffmpeg::renderSequence(renderable.project, request, {}, {}, &summary).ok());
    CHECK(summary.framesEncoded == 20);

    // The ladder fixture encodes each frame's index in its luma, so the first
    // frame out proves the range was honoured rather than merely accepted.
    auto decoder = platform::ffmpeg::openVideoDecoder(request.outputPath);
    REQUIRE(decoder);
    auto frame = (*decoder)->frameAtIndex(0);
    REQUIRE(frame);
    const std::uint16_t luma = frame->sampleAt(frame->width() / 2, frame->height() / 2, 0);
    // 16 + 4*(100 % 55) = 196, expanded from limited to full range on the way
    // out through the display transfer.
    INFO("luma " << luma << " at the first rendered frame");
    CHECK(luma > 180);
}

TEST_CASE("Rendering an empty range is refused", "[render][export]") {
    ZARO_REQUIRE_FIXTURE("ladder_prores.mov");
    Renderable renderable = buildProject("ladder_prores.mov");
    REQUIRE(renderable.ok);

    platform::ffmpeg::RenderRequest request;
    request.outputPath = outputPath("render-empty.mov");
    request.sequence = renderable.sequence;
    request.startFrame = 100000;
    const auto status = platform::ffmpeg::renderSequence(renderable.project, request);
    CHECK_FALSE(status.ok());
}
