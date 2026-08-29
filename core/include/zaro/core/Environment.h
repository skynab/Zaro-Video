#pragma once

#include <optional>
#include <string>

namespace zaro {

/// The value of an environment variable, or nothing when it is not set.
///
/// Wrapped rather than called directly because MSVC deprecates std::getenv
/// (C4996): the pointer it hands back can be invalidated by another thread's
/// putenv, so the Windows runtime offers _dupenv_s, which returns a copy the
/// caller owns. Returning a std::string is that copy on every platform, and
/// leaves callers one answer with one lifetime rather than two spellings.
///
/// An unset variable and one set to "" are different answers, which is why
/// this is an optional: a caller that only reacts to a variable being present
/// would otherwise have to guess.
[[nodiscard]] std::optional<std::string> environmentValue(const char* name);

}  // namespace zaro
