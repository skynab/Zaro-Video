// Renders a project to a file, headless.
//
// This is the plan's claim made concrete: composite(t) is pure, so export is
// nothing more than calling it as fast as possible. The same function will be
// driven by a clock for playback, which is why there is no separate export
// renderer to keep in agreement with the playback one.

#include <QGuiApplication>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "Cli.h"
#include "zaro/core/io/ProjectIo.h"
#include "zaro/core/render/AudioGraph.h"
#include "zaro/core/render/RenderGraph.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"
#include "zaro/platform/qtext/QtTextRasterizer.h"

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
    std::puts("  --version         print the version and exit");
}

}  // namespace

int main(int argc, char** argv) {
    if (zaro::tools::handledVersion(argc, argv, "zaro-render")) {
        return 0;
    }
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
    if (frameCount < 0) {
        frameCount = sequence->duration().frames() - startFrame;
    }
    if (frameCount <= 0) {
        std::fprintf(stderr, "zaro-render: nothing to render\n");
        return 1;
    }

    zaro::platform::ffmpeg::RenderRequest request;
    request.outputPath = outputPath;
    request.sequence = sequence->id();
    request.startFrame = startFrame;
    request.frameCount = frameCount;
    request.includeAudio = includeAudio;
    request.cacheBudgetBytes = cacheMegabytes * 1024u * 1024u;

    // The same function the export dialog calls. Two loops doing this would
    // have to be kept agreeing, and the one nobody runs would be the one that
    // drifted.
    std::int64_t lastReported = -1;
    const auto onProgress = [&](const zaro::platform::ffmpeg::RenderProgress& progress) {
        if (quiet) {
            return;
        }
        if (progress.framesDone == progress.framesTotal ||
            progress.framesDone - lastReported >= 50) {
            lastReported = progress.framesDone;
            std::printf("\r  %lld / %lld frames", static_cast<long long>(progress.framesDone),
                        static_cast<long long>(progress.framesTotal));
            std::fflush(stdout);
        }
    };

    zaro::platform::ffmpeg::RenderSummary summary;
    // Qt's font engine needs an application object even with no window. Built
    // here rather than inside the rasteriser: one per process, and a library
    // that constructs one behind its caller's back is a library that fights
    // whatever the caller already made.
    QGuiApplication fonts{argc, argv};
    zaro::platform::qtext::QtTextRasterizer text;

    const auto began = std::chrono::steady_clock::now();
    if (const auto status = zaro::platform::ffmpeg::renderSequence(loaded->project, request,
                                                                   onProgress, {}, &summary, &text);
        !status) {
        std::fprintf(stderr, "\nzaro-render: %s\n", status.error().toString().c_str());
        return 1;
    }

    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    if (!quiet) {
        std::printf("\n%s\n", outputPath.c_str());
        if (summary.textLayersSkipped > 0) {
            std::printf("  WARNING: %lld text layers were not drawn\n",
                        static_cast<long long>(summary.textLayersSkipped));
        }
        std::printf("  %s: %s\n", summary.copied ? "copied" : "re-encoded",
                    summary.copyReason.c_str());
        std::printf("  %lld frames encoded, %lld packets written\n",
                    static_cast<long long>(summary.framesEncoded),
                    static_cast<long long>(summary.videoPacketsWritten));
        std::printf("  %lld frames at %s in %.2fs (%.1f fps, %.2fx realtime)\n",
                    static_cast<long long>(frameCount), rate.toString().c_str(), elapsed,
                    static_cast<double>(frameCount) / elapsed,
                    static_cast<double>(frameCount) / elapsed / rate.toDouble());
        if (includeAudio) {
            std::printf(
                "  %lld audio samples written, %lld expected -- drift %lld\n",
                static_cast<long long>(summary.audioSamplesWritten),
                static_cast<long long>(summary.audioSamplesExpected),
                static_cast<long long>(summary.audioSamplesWritten - summary.audioSamplesExpected));
        }
        std::printf("  frame cache: %llu hits, %llu misses\n",
                    static_cast<unsigned long long>(summary.cacheHits),
                    static_cast<unsigned long long>(summary.cacheMisses));
    }
    return 0;
}
