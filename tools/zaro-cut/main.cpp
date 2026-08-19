// Builds a project from media files laid end to end.
//
// The command-line equivalent of dragging clips into a timeline. It exists so
// that the render path can be exercised without a UI, and so the sync
// verification has something real to render.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"

namespace {

void printUsage() {
    std::puts("usage: zaro-cut <output.zaro> <media>... [options]");
    std::puts("");
    std::puts("  --rate <r>      sequence frame rate (default: the first clip's)");
    std::puts("  --size <w>x<h>  sequence frame size (default: the first clip's)");
    std::puts("  --seconds <n>   use only the first n seconds of each clip");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        printUsage();
        return 2;
    }

    const std::string outputPath = argv[1];
    std::vector<std::string> inputs;
    zaro::time::Rational requestedRate{0, 1};
    std::int32_t width = 0;
    std::int32_t height = 0;
    double secondsPerClip = 0.0;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--rate" && i + 1 < argc) {
            if (const auto parsed = zaro::time::Rational::parse(argv[++i])) {
                requestedRate = *parsed;
            }
        } else if (arg == "--size" && i + 1 < argc) {
            std::sscanf(argv[++i], "%dx%d", &width, &height);
        } else if (arg == "--seconds" && i + 1 < argc) {
            secondsPerClip = std::atof(argv[++i]);
        } else {
            inputs.push_back(arg);
        }
    }
    if (inputs.empty()) {
        printUsage();
        return 2;
    }

    zaro::platform::ffmpeg::installLogHandler(false);

    zaro::model::Project project;
    zaro::edit::CommandStack stack;

    // Probe everything first so the sequence can take its format from the
    // footage, the way an editor does when you drop the first clip in.
    struct Source {
        zaro::model::MediaRefId id;
        zaro::media::MediaInfo info;
    };
    std::vector<Source> sources;

    for (const std::string& path : inputs) {
        auto probed = zaro::platform::ffmpeg::probe(path);
        if (!probed) {
            std::fprintf(stderr, "zaro-cut: %s\n", probed.error().toString().c_str());
            return 1;
        }
        zaro::model::MediaRef ref;
        ref.id = project.ids().next<zaro::model::MediaRefTag>();
        ref.path = path;
        ref.name = path.substr(path.find_last_of('/') + 1);
        ref.info = *probed;
        sources.push_back({project.addMedia(ref), *probed});
    }

    const zaro::media::VideoStreamInfo* firstVideo = sources.front().info.primaryVideo();
    zaro::time::Rational rate =
        requestedRate.isPositive()
            ? requestedRate
            : (firstVideo != nullptr ? firstVideo->frameRate : zaro::time::rates::fps25);
    if (width <= 0 || height <= 0) {
        width = firstVideo != nullptr ? firstVideo->width : 1920;
        height = firstVideo != nullptr ? firstVideo->height : 1080;
    }
    // Subsampled encoders need even dimensions, and silently producing a file
    // nothing can open is worse than rounding here.
    width -= width % 2;
    height -= height % 2;

    zaro::model::Sequence sequence{project.ids().next<zaro::model::SequenceTag>(), "Sequence 01",
                                   rate};
    sequence.setSize(width, height);
    const auto sequenceId = sequence.id();
    const auto videoTrack = project.ids().next<zaro::model::TrackTag>();
    const auto audioTrack = project.ids().next<zaro::model::TrackTag>();
    sequence.addTrack(videoTrack, zaro::model::TrackKind::Video, "V1");
    sequence.addTrack(audioTrack, zaro::model::TrackKind::Audio, "A1");
    project.addSequence(std::move(sequence));

    zaro::time::RationalTime playhead{0, rate};

    for (const Source& source : sources) {
        const zaro::media::VideoStreamInfo* video = source.info.primaryVideo();
        const zaro::time::Rational sourceRate = video != nullptr ? video->frameRate : rate;

        zaro::time::Rational useSeconds = source.info.duration;
        if (secondsPerClip > 0.0) {
            const auto limit = zaro::time::Rational::approximate(secondsPerClip);
            if (limit < useSeconds) {
                useSeconds = limit;
            }
        }
        if (!useSeconds.isPositive()) {
            continue;
        }

        const auto durationOnTimeline = zaro::time::RationalTime::fromSeconds(useSeconds, rate);
        if (durationOnTimeline.frames() <= 0) {
            continue;
        }

        zaro::model::Clip clip;
        clip.id = project.ids().next<zaro::model::ClipTag>();
        clip.source = source.id;
        clip.name = source.info.path;
        clip.sourceRange =
            zaro::time::TimeRange{zaro::time::RationalTime{0, sourceRate},
                                  zaro::time::RationalTime::fromSeconds(useSeconds, sourceRate)};
        clip.timelineRange = zaro::time::TimeRange{playhead, durationOnTimeline};

        const bool hasVideo = video != nullptr;
        const auto target = hasVideo ? videoTrack : audioTrack;
        auto built = zaro::edit::makeOverwrite(project, {sequenceId, target}, clip);
        if (!built) {
            std::fprintf(stderr, "zaro-cut: %s\n", built.error().toString().c_str());
            return 1;
        }
        stack.execute(project, std::move(*built));

        // A file with both streams gets a linked audio clip on A1. Real A/V
        // linking is Phase 4 work; this is enough to render sound alongside
        // picture.
        if (hasVideo && source.info.primaryAudio() != nullptr) {
            zaro::model::Clip audioClip = clip;
            audioClip.id = project.ids().next<zaro::model::ClipTag>();
            auto audioBuilt =
                zaro::edit::makeOverwrite(project, {sequenceId, audioTrack}, audioClip);
            if (audioBuilt) {
                stack.execute(project, std::move(*audioBuilt));

                // Picture and sound from one file arrive together and should
                // stay together, so they go on as a link group rather than as
                // two clips that happen to line up.
                auto linked = zaro::edit::makeLinkClips(
                    project, sequenceId, {{videoTrack, clip.id}, {audioTrack, audioClip.id}});
                if (linked) {
                    stack.execute(project, std::move(*linked));
                }
            }
        }

        playhead = playhead + durationOnTimeline;
    }

    if (const auto status = zaro::io::saveProject(project, outputPath); !status) {
        std::fprintf(stderr, "zaro-cut: %s\n", status.error().toString().c_str());
        return 1;
    }

    const zaro::model::Sequence* built = project.findSequence(sequenceId);
    std::printf(
        "%s: %dx%d @ %s, %lld frames, %zu clips\n", outputPath.c_str(), width, height,
        rate.toString().c_str(), static_cast<long long>(built->duration().frames()),
        built->videoTracks().front().clips().size() + built->audioTracks().front().clips().size());
    return 0;
}
