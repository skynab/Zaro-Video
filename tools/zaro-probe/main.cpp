// Reads a media file's structure and prints it.
//
// The point of this tool is not convenience -- ffprobe exists -- but proof: it
// exercises our own probe path end to end, so what it prints is exactly what
// the editor will believe about a file.

#include <cstdio>
#include <string>

#include "zaro/platform/ffmpeg/FFmpegMedia.h"

namespace {

void printUsage() {
    std::puts("usage: zaro-probe <file> [--verbose]");
}

std::string describeRate(const zaro::time::Rational& rate) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s (%.4f fps)", rate.toString().c_str(),
                  rate.toDouble());
    return buffer;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 2;
    }
    const std::string path = argv[1];
    bool verbose = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string{argv[i]} == "--verbose") {
            verbose = true;
        }
    }
    zaro::platform::ffmpeg::installLogHandler(verbose);

    const auto probed = zaro::platform::ffmpeg::probe(path);
    if (!probed) {
        std::fprintf(stderr, "zaro-probe: %s\n", probed.error().toString().c_str());
        return 1;
    }
    const zaro::media::MediaInfo& info = *probed;

    std::printf("%s\n", info.path.c_str());
    std::printf("  container    %s\n", info.formatName.c_str());
    std::printf("  duration     %.3fs\n", info.duration.toDouble());
    if (info.bitRate > 0) {
        std::printf("  bitrate      %lld kb/s\n", static_cast<long long>(info.bitRate / 1000));
    }

    for (const auto& video : info.videoStreams) {
        std::printf("\n  video stream %d\n", video.streamIndex);
        std::printf("    codec      %s\n", video.codecName.c_str());
        std::printf("    size       %dx%d\n", video.width, video.height);
        std::printf("    pixels     %s\n", zaro::media::toString(video.pixelFormat));
        std::printf("    rate       %s\n", describeRate(video.frameRate).c_str());
        if (video.isVariableFrameRate) {
            std::printf("    average    %s  [variable frame rate]\n",
                        describeRate(video.averageFrameRate).c_str());
        }
        std::printf("    color      %s%s\n", zaro::media::toString(video.color).c_str(),
                    video.colorWasGuessed ? "  [inferred, not tagged]" : "");
        if (video.pixelAspect != zaro::time::Rational{1, 1}) {
            std::printf("    par        %s  [anamorphic]\n", video.pixelAspect.toString().c_str());
        }
        if (video.rotationDegrees != 0) {
            std::printf("    rotation   %d degrees\n", video.rotationDegrees);
        }
        if (video.startTimecode) {
            std::printf("    timecode   %s\n", video.startTimecode->toString().c_str());
        }
        if (video.frameCountHint > 0) {
            std::printf("    frames     %lld  [container hint]\n",
                        static_cast<long long>(video.frameCountHint));
        }
    }

    for (const auto& audio : info.audioStreams) {
        std::printf("\n  audio stream %d\n", audio.streamIndex);
        std::printf("    codec      %s\n", audio.codecName.c_str());
        std::printf("    rate       %s Hz\n", audio.sampleRate.toString().c_str());
        std::printf("    channels   %d  %s\n", audio.channelCount, audio.channelLayout.c_str());
        std::printf("    duration   %.3fs\n", audio.duration.toDouble());
    }
    return 0;
}
