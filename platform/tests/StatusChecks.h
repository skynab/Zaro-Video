#pragma once

#include <string>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/Error.h"

#include "QtMessageLog.h"

namespace zaro::testing {

/// The reason a `Status` failed, as one line for a test log.
///
/// `REQUIRE(thing().ok())` prints "false" and throws the message away, which
/// on a machine nobody can attach a debugger to -- a CI runner with a GPU
/// backend none of us has -- is the difference between a diagnosis and a
/// guess.
inline std::string why(const Status& status) {
    if (status.ok()) {
        return "ok";
    }
    return std::string{toString(status.error().code())} + ": " + status.error().message();
}

}  // namespace zaro::testing

/// `REQUIRE(expr.ok())`, but the failure says what went wrong -- ours in the
/// Status, and Qt's own account of it underneath, which is where QRhi puts the
/// reason a device would not do something.
#define ZARO_REQUIRE_OK(expr)                                    \
    do {                                                         \
        const ::zaro::Status zaroStatus = (expr);                \
        INFO(#expr << " -> " << ::zaro::testing::why(zaroStatus) \
                   << ::zaro::testing::takeQtMessages());        \
        REQUIRE(zaroStatus.ok());                                \
    } while (false)

/// The non-fatal counterpart.
#define ZARO_CHECK_OK(expr)                                      \
    do {                                                         \
        const ::zaro::Status zaroStatus = (expr);                \
        INFO(#expr << " -> " << ::zaro::testing::why(zaroStatus) \
                   << ::zaro::testing::takeQtMessages());        \
        CHECK(zaroStatus.ok());                                  \
    } while (false)
