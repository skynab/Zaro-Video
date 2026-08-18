#include "zaro/core/Check.h"

#include <cstdio>
#include <cstdlib>

namespace zaro::detail {

void checkFailed(std::string_view condition, std::string_view message, std::string_view file,
                 int line) {
    std::fprintf(stderr,
                 "\nzaro: check failed\n"
                 "  %.*s:%d\n"
                 "  condition: %.*s\n"
                 "  %.*s\n\n",
                 static_cast<int>(file.size()), file.data(), line,
                 static_cast<int>(condition.size()), condition.data(),
                 static_cast<int>(message.size()), message.data());
    std::fflush(stderr);
    std::abort();
}

}  // namespace zaro::detail
