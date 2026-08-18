#pragma once

#include <string_view>

namespace zaro {
namespace detail {
[[noreturn]] void checkFailed(std::string_view condition, std::string_view message,
                              std::string_view file, int line);
}  // namespace detail
}  // namespace zaro

/// An invariant that is enforced in every build configuration, release
/// included.
///
/// Reserved for conditions where continuing would silently corrupt time or
/// media data -- integer overflow in rational arithmetic, for instance. A
/// wrong-but-plausible frame number that propagates into an edit is far worse
/// than a loud stop, because it surfaces days later as drift nobody can trace.
///
/// Ordinary preconditions that a caller can be trusted to honour should use
/// assert() instead, so they cost nothing in shipping builds.
#define ZARO_CHECK(cond, message)                                              \
    do {                                                                       \
        if (!(cond)) [[unlikely]] {                                            \
            ::zaro::detail::checkFailed(#cond, (message), __FILE__, __LINE__); \
        }                                                                      \
    } while (false)
