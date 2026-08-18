// Renders a project to a file, headless.
//
// This is the plan's claim made concrete: composite(t) is pure, so export is
// nothing more than calling it as fast as possible. The same function will be
// driven by a clock for playback, which is why there is no separate export
// renderer to keep in agreement with the playback one.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "zaro/core/io/ProjectIo.h"
#include "zaro/core/render/AudioGraph.h"
#include "zaro/core/render/RenderGraph.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"

namespace {

void printUsage() {
    std::puts("usage: zaro-render <project.zaro> <output.mov|.mp4> [options]");
    std::puts("");
    std::puts("  --start <frame>   first frame to render (default 0)");
    std::puts("  --frames <n>      how many frames (default: the whole sequence)");
    std::puts("  --no-audio        render picture only");
    std::puts("  --cache-mb <n>    frame cache budget in MB (default 64; export is");
    std::puts("                    linear, so a big cache buys nothing here)");
    std::puts("  --quiet           no progress output");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        printUsage();
        return 2;
    }

    const std::string projectPath = argv[1];
    const std::string outputPath = argv[2];
    std::int64_t startFrame = 0;
    std::int64_t frameCount = -1;
    // Export walks the timeline once and never revisits a frame, so a large
    // cache holds hundreds of megabytes for no hits at all. The cache earns
    // its budget when scrubbing, not here.
    std::size_t cacheMegabytes = 64;
    bool includeAudio = true;
    bool quiet = false;

    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--start" && i + 1 < argc) {
            startFrame = std::atoll(argv[++i]);
        } else if (arg == "--frames" && i + 1 < argc) {
            frameCount = std::atoll(argv[++i]);
        } else if (arg == "--cache-mb" && i + 1 < argc) {
            cacheMegabytes = static_cast<std::size_t>(std::atoll(argv[++i]));
        } else if (arg == "--no-audio") {
            includeAudio = false;
        } else if (arg == "--quiet") {
            quiet = true;
        } else {
            std::fprintf(stderr, "zaro-render: unknown option '%s'\n", arg.c_str());
            return 2;
        }
    }

    zaro::platform::ffmpeg::installLogHandler(false);

    auto loaded = zaro::io::loadProject(projectPath);
    if (!loaded) {
        std::fprintf(stderr, "zaro-render: %s\n", loaded.error().toString().c_str());
        return 1;
    }
    const zaro::model::Project& project = loaded->project;
    const zaro::model::Sequence* sequence = project.findSequence(project.activeSequence());
    if (sequence == nullptr) {
        std::fprintf(stderr, "zaro-render: this project has no active sequence\n");
        return 1;
    }

    const zaro::time::Rational& rate = sequence->frameRate();
    const zaro::time::Rational& audioRate = sequence->audioSampleRate();
    if (frameCount < 0) {
        frameCount = sequence->duration().frames() - startFrame;
    }
    if (frameCount <= 0) {
        std::fprintf(stderr, "zaro-render: nothing to render\n");
        return 1;
    }

    auto sourceOpened =
        zaro::platform::ffmpeg::ProjectMediaSource::open(project, cacheMegabytes * 1024u * 1024u);
    if (!sourceOpened) {
        std::fprintf(stderr, "zaro-render: %s\n", sourceOpened.error().toString().c_str());
        return 1;
    }
    zaro::platform::ffmpeg::ProjectMediaSource& source = **sourceOpened;

    zaro::render::RenderGraph video{source};
    zaro::render::AudioGraph audio{source};

    zaro::platform::ffmpeg::EncodeSettings settings;
    settings.path = outputPath;
    settings.width = sequence->width();
    settings.height = sequence->height();
    settings.frameRate = rate;
    settings.audioSampleRate = audioRate;
    settings.includeAudio = includeAudio;

    auto encoderOpened = zaro::platform::ffmpeg::Encoder::open(settings);
    if (!encoderOpened) {
        std::fprintf(stderr, "zaro-render: %s\n", encoderOpened.error().toString().c_str());
        return 1;
    }
    zaro::platform::ffmpeg::Encoder& encoder = **encoderOpened;

    // Audio is addressed by an exact rational relationship to the frame number,
    // not by adding a per-frame duration to a running total. At 29.97 there are
    // 1601.6 samples per frame; accumulating that as a rounded integer drifts by
    // a sample every few frames, and by a visible lip-sync error over an hour.
    const zaro::time::Rational samplesPerFrame = audioRate / rate;
    const auto sampleAtFrame = [&](std::int64_t frame) {
        return (zaro::time::Rational::fromInt(frame) * samplesPerFrame).floorToInt();
    };

    zaro::render::RgbaImage frame;
    const auto began = std::chrono::steady_clock::now();

    for (std::int64_t index = 0; index < frameCount; ++index) {
        const std::int64_t timelineFrame = startFrame + index;
        const zaro::time::RationalTime at{timelineFrame, rate};

        if (const auto status = video.compositeInto(*sequence, at, frame); !status) {
            std::fprintf(stderr, "zaro-render: frame %lld: %s\n",
                         static_cast<long long>(timelineFrame), status.error().toString().c_str());
            return 1;
        }
        if (const auto status = encoder.writeVideo(frame); !status) {
            std::fprintf(stderr, "zaro-render: %s\n", status.error().toString().c_str());
            return 1;
        }

        if (includeAudio) {
            const std::int64_t from = sampleAtFrame(startFrame + index);
            const std::int64_t to = sampleAtFrame(startFrame + index + 1);
            auto mixed = audio.mix(*sequence, zaro::time::RationalTime{from, audioRate}, to - from);
            if (!mixed) {
                std::fprintf(stderr, "zaro-render: %s\n", mixed.error().toString().c_str());
                return 1;
            }
            if (const auto status = encoder.writeAudio(*mixed); !status) {
                std::fprintf(stderr, "zaro-render: %s\n", status.error().toString().c_str());
                return 1;
            }
        }

        if (!quiet && (index % 50 == 0 || index + 1 == frameCount)) {
            std::printf("\r  %lld / %lld frames", static_cast<long long>(index + 1),
                        static_cast<long long>(frameCount));
            std::fflush(stdout);
        }
    }

    if (const auto status = encoder.finish(); !status) {
        std::fprintf(stderr, "\nzaro-render: %s\n", status.error().toString().c_str());
        return 1;
    }

    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    if (!quiet) {
        const std::int64_t expectedSamples =
            sampleAtFrame(startFrame + frameCount) - sampleAtFrame(startFrame);
        std::printf("\n%s\n", outputPath.c_str());
        std::printf("  %lld frames encoded, %lld packets written\n",
                    static_cast<long long>(encoder.framesWritten()),
                    static_cast<long long>(encoder.videoPacketsWritten()));
        std::printf("  %lld frames at %s in %.2fs (%.1f fps, %.2fx realtime)\n",
                    static_cast<long long>(encoder.framesWritten()), rate.toString().c_str(),
                    elapsed, static_cast<double>(frameCount) / elapsed,
                    static_cast<double>(frameCount) / elapsed / rate.toDouble());
        if (includeAudio) {
            std::printf("  %lld audio samples written, %lld expected -- drift %lld\n",
                        static_cast<long long>(encoder.samplesWritten()),
                        static_cast<long long>(expectedSamples),
                        static_cast<long long>(encoder.samplesWritten() - expectedSamples));
        }
        std::printf("  frame cache: %llu hits, %llu misses, %zu MB held\n",
                    static_cast<unsigned long long>(source.cache().hits()),
                    static_cast<unsigned long long>(source.cache().misses()),
                    source.cache().byteSize() / (1024u * 1024u));
    }
    return 0;
}
