#pragma once

#include <string>

namespace zaro::testing {

/// Everything Qt has written to its own log since this was last called, and
/// clears it.
///
/// QRhi reports why a pipeline, a texture or a shader would not be made
/// through qWarning, and returns nothing but false to its caller. On a machine
/// nobody can attach a debugger to, that text is the whole diagnosis -- a
/// pipeline that will not build says so with the compiler's own errors, and a
/// Status carrying "cannot create a graphics pipeline" does not.
[[nodiscard]] std::string takeQtMessages();

}  // namespace zaro::testing
