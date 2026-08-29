#include <cstdint>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"

#include "Fixtures.h"

using namespace zaro;
using zaro::testing::fixture;

namespace {

std::string scratch(const char* name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove(path);
    return path.string();
}

/// One untouched clip of a real file, at the file's own size and rate: the
/// case a copy exists for.
struct OneShot {
    model::Project project;
    model::SequenceId sequenceId;
    model::ClipId clipId;
    bool ok{false};

    explicit OneShot(const std::string& path) {
        auto probed = platform::ffmpeg::probe(path);
        if (!probed || probed->primaryVideo() == nullptr) {
            return;
        }
        const media::VideoStreamInfo& video = *probed->primaryVideo();

        model::MediaRef ref;
        ref.id = project.ids().next<model::MediaRefTag>();
        ref.path = path;
        ref.name = "shot";
        ref.info = *probed;
        const auto mediaId = project.addMedia(ref);

        model::Sequence sequence{project.ids().next<model::SequenceTag>(), "Sequence",
                                 video.frameRate};
        sequence.setSize(video.width, video.height);
        sequenceId = sequence.id();
        const auto trackId = project.ids().next<model::TrackTag>();
        sequence.addTrack(trackId, model::TrackKind::Video, "V1");

        model::Clip clip;
        clip.id = project.ids().next<model::ClipTag>();
        clip.source = mediaId;
        clip.timelineRange = time::TimeRange{time::RationalTime{0, video.frameRate},
                                             time::RationalTime{50, video.frameRate}};
        clip.sourceRange = clip.timelineRange;
        clipId = clip.id;
        trackIdValue = trackId;
        sequence.findTrack(trackId)->insert(std::move(clip));
        project.addSequence(std::move(sequence));
        ok = true;
    }

    model::TrackId trackIdValue;
};

}  // namespace

TEST_CASE("an untouched cut of a file is exported by copying it", "[smartexport]") {
    ZARO_REQUIRE_FIXTURE("ladder_prores.mov");
    OneShot world{fixture("ladder_prores.mov")};
    REQUIRE(world.ok);

    const std::string out = scratch("zaro-smart-copy.mov");
    platform::ffmpeg::RenderRequest request;
    request.outputPath = out;
    request.sequence = world.sequenceId;
    request.frameCount = 50;
    request.includeAudio = false;

    platform::ffmpeg::RenderSummary summary;
    REQUIRE(platform::ffmpeg::renderSequence(world.project, request, {}, {}, &summary));
    INFO(summary.copyReason);
    CHECK(summary.copied);
    CHECK(summary.videoPacketsWritten == 50);

    // Same codec, same size, same length: a copy that changed any of those
    // would not be a copy.
    auto probed = platform::ffmpeg::probe(out);
    REQUIRE(probed);
    REQUIRE(probed->primaryVideo() != nullptr);
    CHECK(probed->primaryVideo()->codecName == "prores");
    CHECK(probed->primaryVideo()->width == 320);
    CHECK(probed->primaryVideo()->durationInFrames().frames() == 50);

    std::filesystem::remove(out);
}

TEST_CASE("a copied export decodes to the same pixels as the source", "[smartexport]") {
    ZARO_REQUIRE_FIXTURE("ladder_prores.mov");
    OneShot world{fixture("ladder_prores.mov")};
    REQUIRE(world.ok);

    const std::string out = scratch("zaro-smart-pixels.mov");
    platform::ffmpeg::RenderRequest request;
    request.outputPath = out;
    request.sequence = world.sequenceId;
    request.frameCount = 50;
    request.includeAudio = false;
    platform::ffmpeg::RenderSummary summary;
    REQUIRE(platform::ffmpeg::renderSequence(world.project, request, {}, {}, &summary));
    REQUIRE(summary.copied);

    // The frame ladder encodes its own index in its luma, so "the same frames,
    // in the same order" is a claim that can be checked from the pixels rather
    // than inferred from the packet count.
    OneShot copied{out};
    REQUIRE(copied.ok);
    auto sourceReader = platform::ffmpeg::ProjectMediaSource::open(world.project);
    auto copyReader = platform::ffmpeg::ProjectMediaSource::open(copied.project);
    REQUIRE(sourceReader);
    REQUIRE(copyReader);

    const auto rate = time::rates::fps25;
    for (const std::int64_t frame : {0, 7, 24, 49}) {
        auto was =
            (*sourceReader)
                ->imageFor(world.project.media().front().id, time::RationalTime{frame, rate});
        auto now =
            (*copyReader)
                ->imageFor(copied.project.media().front().id, time::RationalTime{frame, rate});
        REQUIRE(was);
        REQUIRE(now);
        REQUIRE((*was)->width() == (*now)->width());
        // Bit for bit: nothing was decoded and re-encoded, so nothing can have
        // moved by a code value.
        bool identical = true;
        for (std::int32_t y = 0; y < (*was)->height() && identical; ++y) {
            for (std::int32_t x = 0; x < (*was)->width(); ++x) {
                if (!((*was)->at(x, y) == (*now)->at(x, y))) {
                    identical = false;
                    break;
                }
            }
        }
        INFO("frame " << frame);
        CHECK(identical);
    }

    // Windows will not unlink a file that still has an open handle, and the
    // reader holds one on the copy until it is destroyed.
    copyReader->reset();
    std::filesystem::remove(out);
}

TEST_CASE("a graded cut is re-encoded, and says why", "[smartexport]") {
    ZARO_REQUIRE_FIXTURE("ladder_prores.mov");
    OneShot world{fixture("ladder_prores.mov")};
    REQUIRE(world.ok);
    world.project.findSequence(world.sequenceId)
        ->findTrack(world.trackIdValue)
        ->find(world.clipId)
        ->color.exposure = 1.0;

    const std::string out = scratch("zaro-smart-graded.mov");
    platform::ffmpeg::RenderRequest request;
    request.outputPath = out;
    request.sequence = world.sequenceId;
    request.frameCount = 10;
    request.includeAudio = false;

    platform::ffmpeg::RenderSummary summary;
    REQUIRE(platform::ffmpeg::renderSequence(world.project, request, {}, {}, &summary));
    CHECK_FALSE(summary.copied);
    CHECK(summary.copyReason.find("graded") != std::string::npos);
    CHECK(summary.framesEncoded == 10);

    std::filesystem::remove(out);
}

TEST_CASE("copying can be switched off", "[smartexport]") {
    ZARO_REQUIRE_FIXTURE("ladder_prores.mov");
    OneShot world{fixture("ladder_prores.mov")};
    REQUIRE(world.ok);

    const std::string out = scratch("zaro-smart-forced.mov");
    platform::ffmpeg::RenderRequest request;
    request.outputPath = out;
    request.sequence = world.sequenceId;
    request.frameCount = 10;
    request.includeAudio = false;
    request.allowCopy = false;

    platform::ffmpeg::RenderSummary summary;
    REQUIRE(platform::ffmpeg::renderSequence(world.project, request, {}, {}, &summary));
    CHECK_FALSE(summary.copied);
    CHECK(summary.framesEncoded == 10);

    std::filesystem::remove(out);
}
