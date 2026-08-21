#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"
#include "zaro/core/render/ColorPipeline.h"
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

TEST_CASE("The sequence's delivery reaches the exported file", "[render][export][tonemap]") {
    // The division of labour, end to end: the encoder writes what it is given
    // and clips what does not fit, and the rolloff is what makes sure nothing
    // needs clipping. Checked through a real encode, because the two live in
    // different libraries and nothing else would notice if the sequence's
    // setting stopped being read on the way.
    Renderable renderable = buildProject("sync_click_flash.mov");
    if (!renderable.ok) {
        SKIP("fixture missing");
    }

    // Push the whole picture well above white, so every frame has something a
    // clipping encoder would flatten.
    model::Sequence* sequence = renderable.project.findSequence(renderable.sequence);
    model::Clip lifted = sequence->videoTracks().front().clips().front();
    lifted.color.exposure = 3.0;  // eight times the light
    sequence->tracksMutable(model::TrackKind::Video).front().setClips({lifted});

    const auto brightestCode = [&](const std::string& path) {
        auto decoder = platform::ffmpeg::openVideoDecoder(path);
        REQUIRE(decoder);
        auto frame = (*decoder)->frameAtTime(time::RationalTime{0, time::rates::fps25});
        REQUIRE(frame);
        render::RgbaImage image;
        REQUIRE(render::toLinear(*frame, image).ok());
        float highest = 0.0F;
        for (std::int32_t y = 0; y < image.height(); ++y) {
            const render::Rgba* row = image.row(y);
            for (std::int32_t x = 0; x < image.width(); ++x) {
                highest = std::max(highest, row[x].g);
            }
        }
        return highest;
    };

    platform::ffmpeg::RenderRequest request;
    request.sequence = renderable.sequence;
    request.startFrame = 0;
    request.frameCount = 2;
    request.includeAudio = false;

    request.outputPath = outputPath("delivery-clipped.mov");
    REQUIRE(platform::ffmpeg::renderSequence(renderable.project, request).ok());
    const float clipped = brightestCode(request.outputPath);

    model::Sequence::Output rolled;
    rolled.highlightKnee = 0.7;
    renderable.project.findSequence(renderable.sequence)->setOutput(rolled);
    request.outputPath = outputPath("delivery-rolled.mov");
    REQUIRE(platform::ffmpeg::renderSequence(renderable.project, request).ok());
    const float mapped = brightestCode(request.outputPath);

    INFO("clipped " << clipped << ", rolled off " << mapped);
    // Clipping puts the highlight on the top code; the rolloff leaves it below,
    // which is the whole observable difference.
    CHECK(clipped >= 0.99F);
    CHECK(mapped < clipped);
    CHECK(mapped > 0.5F);
}
