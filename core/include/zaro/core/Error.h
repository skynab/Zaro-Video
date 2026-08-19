#pragma once

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace zaro {

enum class ErrorCode {
    Unknown,
    NotFound,      ///< No such file, stream or frame.
    Unsupported,   ///< Well-formed, but we do not handle it.
    InvalidData,   ///< Malformed input.
    DecodeFailed,  ///< The decoder rejected otherwise plausible data.
    EndOfStream,   ///< Not a failure; the caller ran off the end.
    Io,            ///< Read/write failure below us.
    Internal,      ///< Our bug.
    Cancelled,     ///< Abandoned on request. Not a failure.
};

[[nodiscard]] const char* toString(ErrorCode code) noexcept;

class Error {
public:
    Error(ErrorCode code, std::string message) : code_{code}, message_{std::move(message)} {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    /// "decode failed: no frame at index 4000"
    [[nodiscard]] std::string toString() const;

private:
    ErrorCode code_;
    std::string message_;
};

/// A value or an explanation of its absence.
///
/// Media I/O fails constantly and unremarkably -- truncated files, codecs we do
/// not handle, seeks past the end -- so failure needs to be an ordinary return
/// value rather than an exception. This is `std::expected` in all but name;
/// it exists because C++23 library support is not yet reliable across the
/// compilers this project targets. The interface is deliberately a subset of
/// `std::expected` so the eventual swap is mechanical.
template <typename T>
class Result {
public:
    Result(T value) : storage_{std::move(value)} {}      // NOLINT(google-explicit-constructor)
    Result(Error error) : storage_{std::move(error)} {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool hasValue() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] T& value() & { return std::get<T>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<T>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<T>(std::move(storage_)); }

    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T&& operator*() && { return std::move(*this).value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    [[nodiscard]] const Error& error() const& { return std::get<Error>(storage_); }

private:
    std::variant<T, Error> storage_;
};

/// The void-returning counterpart.
class Status {
public:
    Status() = default;
    Status(Error error) : error_{std::move(error)} {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const Error& error() const { return *error_; }

private:
    std::optional<Error> error_;
};

}  // namespace zaro
