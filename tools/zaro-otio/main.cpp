#include <cstdio>
#include <cstring>
#include <string>

#include "Cli.h"
#include "zaro/core/io/OtioIo.h"
#include "zaro/core/io/ProjectIo.h"

namespace {

void usage() {
    std::printf(
        "zaro-otio — convert between CutReel projects and OpenTimelineIO\n"
        "\n"
        "  zaro-otio export <project.zaro> <out.otio> [--sequence <id>]\n"
        "  zaro-otio import <in.otio> <project.zaro>\n"
        "  zaro-otio --version\n"
        "\n"
        "Importing writes a project of its own rather than merging into one:\n"
        "an OTIO file names its media by URL, and matching those against media\n"
        "already in a project is a different decision from reading the file.\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (zaro::tools::handledVersion(argc, argv, "zaro-otio")) {
        return 0;
    }
    if (argc < 4) {
        usage();
        return argc < 2 ? 1 : 0;
    }
    const std::string mode = argv[1];
    const std::string from = argv[2];
    const std::string to = argv[3];

    if (mode == "export") {
        auto loaded = zaro::io::loadProject(from);
        if (!loaded) {
            std::fprintf(stderr, "zaro-otio: %s\n", loaded.error().toString().c_str());
            return 1;
        }
        if (loaded->project.sequences().empty()) {
            std::fprintf(stderr, "zaro-otio: that project has no sequences\n");
            return 1;
        }
        zaro::model::SequenceId sequence = loaded->project.sequences().front().id();
        for (int i = 4; i + 1 < argc; ++i) {
            if (std::strcmp(argv[i], "--sequence") == 0) {
                sequence =
                    zaro::model::SequenceId{static_cast<std::uint64_t>(std::stoull(argv[i + 1]))};
            }
        }
        if (const auto status = zaro::io::saveOtio(loaded->project, sequence, to); !status) {
            std::fprintf(stderr, "zaro-otio: %s\n", status.error().toString().c_str());
            return 1;
        }
        std::printf("%s\n", to.c_str());
        return 0;
    }

    if (mode == "import") {
        auto loaded = zaro::io::loadOtio(from);
        if (!loaded) {
            std::fprintf(stderr, "zaro-otio: %s\n", loaded.error().toString().c_str());
            return 1;
        }
        if (const auto status = zaro::io::saveProject(*loaded, to); !status) {
            std::fprintf(stderr, "zaro-otio: %s\n", status.error().toString().c_str());
            return 1;
        }
        const auto& sequence = loaded->sequences().front();
        std::printf("%s\n  %zu video tracks, %zu audio tracks, %zu media at %s\n", to.c_str(),
                    sequence.videoTracks().size(), sequence.audioTracks().size(),
                    loaded->media().size(), sequence.frameRate().toString().c_str());
        return 0;
    }

    usage();
    return 1;
}
