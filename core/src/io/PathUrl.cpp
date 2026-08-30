#include "PathUrl.h"

#include <cctype>
#include <cstddef>
#include <string>

namespace zaro::io {
namespace {

bool isUnreservedPathChar(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '-' || c == '_' || c == '.' || c == '~' || c == '/' ||
           c == '+' || c == ',' || c == '(' || c == ')' || c == '\'' || c == '!' || c == '$' ||
           c == '&' || c == '=' || c == '@' || c == ':';
}

int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

}  // namespace

std::string percentEncodePath(const std::string& path) {
    std::string out;
    out.reserve(path.size());
    for (const char c : path) {
        if (isUnreservedPathChar(c)) {
            out += c;
        } else {
            static constexpr char kHex[] = "0123456789ABCDEF";
            out += '%';
            const auto u = static_cast<unsigned char>(c);
            out += kHex[u >> 4U];
            out += kHex[u & 0x0FU];
        }
    }
    return out;
}

std::string pathFromUrl(const std::string& url) {
    std::string rest = url;
    if (rest.rfind("file://localhost", 0) == 0) {
        rest = rest.substr(16);
    } else if (rest.rfind("file://", 0) == 0) {
        rest = rest.substr(7);
    }
    std::string out;
    out.reserve(rest.size());
    for (std::size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size()) {
            const int hi = hexValue(rest[i + 1]);
            const int lo = hexValue(rest[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += rest[i];
    }
    return out;
}

std::string fileNameOf(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace zaro::io
