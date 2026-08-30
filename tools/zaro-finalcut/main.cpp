#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "zaro/core/io/FinalCutXml.h"
#include "zaro/core/io/ProjectIo.h"

#include "Cli.h"

namespace {

void usage() {
    std::printf(
        "zaro-finalcut — convert between CutReel projects and Final Cut Pro\n"
        "\n"
        "  zaro-finalcut export <project.zaro> <out.fcpxml> [--sequence <id>]\n"
        "  zaro-finalcut import <in.fcpxml> <project.zaro>\n"
        "  zaro-finalcut --version\n"
        "\n"
        "The file is FCPXML, which is what Final Cut Pro reads and writes:\n"
        "File > Import > XML for one this wrote, File > Export XML for one to\n"
        "bring back. A Final Cut library is a bundle of undocumented binary\n"
        "databases that moves with the application version, and is not an\n"
        "interchange format in either direction.\n"
        "\n"
        "This is not the FCP7 XML zaro-premiere writes. The two formats share a\n"
        "vendor and nothing else: Final Cut cannot read xmeml and Premiere\n"
        "cannot read .fcpxml, so both tools exist.\n"
        "\n"
        "Importing reads a .fcpxml file or the .fcpxmld bundle around one, and\n"
        "writes a project of its own rather than merging into one: the file\n"
        "names its media by path, and matching those against media already in a\n"
        "project is a different decision from reading the file.\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (zaro::tools::handledVersion(argc, argv, "zaro-finalcut")) {
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
            std::fprintf(stderr, "zaro-finalcut: %s\n", loaded.error().toString().c_str());
            return 1;
        }
        if (loaded->project.sequences().empty()) {
            std::fprintf(stderr, "zaro-finalcut: that project has no sequences\n");
            return 1;
        }
        zaro::model::SequenceId sequence = loaded->project.sequences().front().id();
        for (int i = 4; i + 1 < argc; ++i) {
            if (std::strcmp(argv[i], "--sequence") == 0) {
                sequence =
                    zaro::model::SequenceId{static_cast<std::uint64_t>(std::stoull(argv[i + 1]))};
            }
        }
        if (const auto status = zaro::io::saveFcpXml(loaded->project, sequence, to); !status) {
            std::fprintf(stderr, "zaro-finalcut: %s\n", status.error().toString().c_str());
            return 1;
        }
        std::printf("%s\n", to.c_str());
        return 0;
    }

    if (mode == "import") {
        auto loaded = zaro::io::loadFcpXml(from);
        if (!loaded) {
            std::fprintf(stderr, "zaro-finalcut: %s\n", loaded.error().toString().c_str());
            return 1;
        }
        if (const auto status = zaro::io::saveProject(*loaded, to); !status) {
            std::fprintf(stderr, "zaro-finalcut: %s\n", status.error().toString().c_str());
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
