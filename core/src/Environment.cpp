#include "zaro/core/Environment.h"

#include <cstdlib>

namespace zaro {

std::optional<std::string> environmentValue(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string copy{value};
    // _dupenv_s allocates with malloc and says so; the caller frees it.
    std::free(value);
    return copy;
#else
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string{value};
#endif
}

}  // namespace zaro
