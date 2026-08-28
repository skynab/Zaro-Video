// Extracts a single frame from a media file.
//
// This is the tool the Phase 1 exit criterion is written against: its output
// must be byte-identical to FFmpeg's for the same frame, which is what proves
// seeking and decoding are frame-exact rather than approximately right.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "zaro/core/time/Timecode.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"

#include "Cli.h"

namespace {

void printUsage() {
    std::puts("usage: zaro-frame <input> <frame-index> <output.png> [options]");
    std::puts("       zaro-frame <input> --time <timecode> <output> [options]");
    std::puts("");
    std::puts("  --raw          write packed planes instead of a PNG");
    std::puts("  --software     force software decoding");
    std::puts("  --hardware     require hardware decoding");
    std::puts("  --benchmark N  decode N frames sequentially and report throughput");
    std::puts("  --quiet        suppress the per-frame report");
    std::puts("  --version       print the version and exit");
}

}  // namespace

int main(int argc, char** argv) {
    if (zaro::tools::handledVersion(argc, argv, "zaro-frame")) {
        return 0;
    }
    if (argc < 3) {
        printUsage();
        return 2;
    }

    const std::string input = argv[1];
    std::string output;
    std::string timecodeText;
    std::int64_t frameIndex = -1;
    bool raw = false;
    bool quiet = false;
    std::int64_t benchmarkFrames = 0;
    zaro::media::DecoderOptions options;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--raw") {
            raw = true;
        } else if (arg == "--quiet") {
            quiet = true;
        } else if (arg == "--software") {
            options.mode = zaro::media::DecodeMode::ForceSoftware;
        } else if (arg == "--hardware") {
            options.mode = zaro::media::DecodeMode::ForceHardware;
        } else if (arg == "--time" && i + 1 < argc) {
            timecodeText = argv[++i];
        } else if (arg == "--benchmark" && i + 1 < argc) {
            benchmarkFrames = std::atoll(argv[++i]);
        } else if (frameIndex < 0 && timecodeText.empty() &&
                   arg.find_first_not_of("0123456789") == std::string::npos) {
            frameIndex = std::atoll(arg.c_str());
        } else {
            output = arg;
        }
    }

    zaro::platform::ffmpeg::installLogHandler(false);

    auto opened = zaro::platform::ffmpeg::openVideoDecoder(input, options);
    if (!opened) {
        std::fprintf(stderr, "zaro-frame: %s\n", opened.error().toString().c_str());
        return 1;
    }
    zaro::media::VideoDecoder& decoder = **opened;

    if (benchmarkFrames > 0) {
        const auto start = std::chrono::steady_clock::now();
        std::int64_t decoded = 0;
        for (; decoded < benchmarkFrames; ++decoded) {
            auto frame = decoder.nextFrame();
            if (!frame) {
                break;
            }
        }
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const double fps = elapsed > 0.0 ? static_cast<double>(decoded) / elapsed : 0.0;
        const double realtime = fps / decoder.info().frameRate.toDouble();
        std::printf("%s: %lld frames in %.2fs -- %.1f fps, %.2fx realtime (%s decode)\n",
                    input.c_str(), static_cast<long long>(decoded), elapsed, fps, realtime,
                    decoder.usingHardware() ? "hardware" : "software");
        return 0;
    }

    if (!timecodeText.empty()) {
        const auto parsed = zaro::time::parseTimecode(timecodeText);
        if (!parsed) {
            std::fprintf(stderr, "zaro-frame: cannot parse timecode '%s'\n", timecodeText.c_str());
            return 2;
        }
        const auto asFrames = zaro::time::framesFromTimecode(*parsed, decoder.info().frameRate);
        if (!asFrames) {
            std::fprintf(stderr, "zaro-frame: timecode '%s' does not exist at %s fps\n",
                         timecodeText.c_str(), decoder.info().frameRate.toString().c_str());
            return 2;
        }
        frameIndex = *asFrames;
    }

    if (frameIndex < 0) {
        printUsage();
        return 2;
    }

    auto frame = decoder.frameAtIndex(frameIndex);
    if (!frame) {
        std::fprintf(stderr, "zaro-frame: %s\n", frame.error().toString().c_str());
        return 1;
    }

    if (!output.empty()) {
        const auto status = raw ? zaro::platform::ffmpeg::writeRawPlanes(*frame, output)
                                : zaro::platform::ffmpeg::writePng(*frame, output);
        if (!status) {
            std::fprintf(stderr, "zaro-frame: %s\n", status.error().toString().c_str());
            return 1;
        }
    }

    if (!quiet) {
        std::printf("frame %lld  %dx%d %s  pts %s  %s  %s decode\n",
                    static_cast<long long>(frameIndex), frame->width(), frame->height(),
                    zaro::media::toString(frame->format()), frame->pts().toString().c_str(),
                    zaro::media::toString(frame->color()).c_str(),
                    decoder.usingHardware() ? "hardware" : "software");
    }
    return 0;
}
