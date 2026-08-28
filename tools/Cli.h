// Argument handling shared by the command line tools.
#pragma once

#include <cstdio>
#include <cstring>

#include <zaro/Version.h>

namespace zaro::tools {

// Prints "<tool> (CutReel) <version>" and says whether it did.
//
// Called before each tool's own argument-count check, on purpose: --version
// answers a question about the build, not about the job, so it has to work on
// a command line that names no input file and would otherwise be a usage
// error.
inline bool handledVersion(int argc, char** argv, const char* tool) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("%s (%.*s) %.*s\n", tool, static_cast<int>(kAppName.size()),
                        kAppName.data(), static_cast<int>(kVersion.size()), kVersion.data());
            return true;
        }
    }
    return false;
}

}  // namespace zaro::tools
