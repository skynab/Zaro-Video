#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace zaro::testing {

/// Absolute path to a generated fixture.
inline std::string fixture(const std::string& name) {
    return (std::filesystem::path{ZARO_TESTDATA_DIR} / name).string();
}

inline bool haveFixture(const std::string& name) {
    return std::filesystem::exists(fixture(name));
}

/// Fixtures are generated rather than checked in, so a fresh clone that has not
/// run testdata/generate.sh should report that clearly instead of failing in a
/// way that looks like a decoder bug.
#define ZARO_REQUIRE_FIXTURE(name)                                                \
    do {                                                                          \
        if (!::zaro::testing::haveFixture(name)) {                                \
            SKIP("missing fixture " << (name) << " -- run testdata/generate.sh"); \
        }                                                                         \
    } while (false)

/// The frame ladder fixtures encode each frame's index in its luma: a decoded
/// frame can be identified from its pixels alone, with no reference decoder in
/// the loop. Steps are four code values apart so that lossy compression noise,
/// which runs to a value or two, can never be mistaken for a neighbouring
/// frame. See testdata/generate.sh.
inline int expectedLadderLuma(std::int64_t frameIndex) {
    return static_cast<int>(16 + 4 * (frameIndex % 55));
}

}  // namespace zaro::testing
