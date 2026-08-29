#include <cstdlib>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/Environment.h"

namespace {

void setVariable(const char* name, const char* value) {
#if defined(_MSC_VER)
    _putenv_s(name, value);
#else
    ::setenv(name, value, 1);
#endif
}

void unsetVariable(const char* name) {
#if defined(_MSC_VER)
    // On Windows, setting a variable to the empty string is how one is
    // removed -- which is also why the empty-value case below is checked
    // everywhere else and not here.
    _putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
}

constexpr const char* kName = "ZARO_TEST_ENVIRONMENT_VALUE";

}  // namespace

TEST_CASE("An unset variable reads as nothing", "[io]") {
    unsetVariable(kName);
    CHECK_FALSE(zaro::environmentValue(kName).has_value());
}

TEST_CASE("A variable reads back the value it was given", "[io]") {
    setVariable(kName, "a value with spaces");
    const auto value = zaro::environmentValue(kName);
    REQUIRE(value.has_value());
    CHECK(*value == "a value with spaces");
    unsetVariable(kName);
}

#if !defined(_MSC_VER)
TEST_CASE("A variable set to nothing is not the same as an unset one", "[io]") {
    // The distinction the optional exists for: present but empty is an answer,
    // and a caller that only reacts to presence needs to see it. Windows has
    // no way to express it -- setting an empty value removes the variable.
    setVariable(kName, "");
    const auto value = zaro::environmentValue(kName);
    REQUIRE(value.has_value());
    CHECK(value->empty());
    unsetVariable(kName);
}
#endif
