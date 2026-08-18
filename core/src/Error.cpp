#include "zaro/core/Error.h"

namespace zaro {

const char* toString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Unknown:
            return "unknown error";
        case ErrorCode::NotFound:
            return "not found";
        case ErrorCode::Unsupported:
            return "unsupported";
        case ErrorCode::InvalidData:
            return "invalid data";
        case ErrorCode::DecodeFailed:
            return "decode failed";
        case ErrorCode::EndOfStream:
            return "end of stream";
        case ErrorCode::Io:
            return "i/o error";
        case ErrorCode::Internal:
            return "internal error";
    }
    return "unknown error";
}

std::string Error::toString() const {
    return std::string{zaro::toString(code_)} + ": " + message_;
}

}  // namespace zaro
