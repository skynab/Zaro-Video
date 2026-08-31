// Folding a wider source into the bus it is cut into.
//
// The bug this replaces was silent and severe: the mixer took the first N
// channels, so a 5.1 file in a stereo sequence kept L and R and dropped C --
// and in a 5.1 mix the centre channel is the dialogue.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/Downmix.h"

using namespace zaro;
using Catch::Approx;
using render::DownmixMatrix;

TEST_CASE("A matched layout passes straight through", "[render][downmix]") {
    for (const std::int32_t channels : {1, 2, 6}) {
        const DownmixMatrix matrix{channels, channels};
        CHECK(matrix.isPassThrough());
        for (std::int32_t i = 0; i < channels; ++i) {
            CHECK(matrix.weight(i, i) == Approx(1.0F));
        }
    }
}

TEST_CASE("A mono source feeds every output", "[render][downmix]") {
    // What the old code did, kept: the pan law is what places a mono source,
    // and changing its level here would move every mono clip in every project.
    const DownmixMatrix matrix{1, 2};
    CHECK(matrix.isPassThrough());
    CHECK(matrix.weight(0, 0) == Approx(1.0F));
    CHECK(matrix.weight(1, 0) == Approx(1.0F));
}

TEST_CASE("5.1 folds to stereo with the dialogue intact", "[render][downmix]") {
    // L R C LFE Ls Rs. The centre reaches both sides at -3 dB, which is the
    // whole point: it is where the words are.
    const DownmixMatrix matrix{6, 2};
    CHECK_FALSE(matrix.isPassThrough());

    CHECK(matrix.weight(0, 0) == Approx(1.0F));                  // L -> left
    CHECK(matrix.weight(1, 1) == Approx(1.0F));                  // R -> right
    CHECK(matrix.weight(0, 2) == Approx(DownmixMatrix::kFold));  // C -> left
    CHECK(matrix.weight(1, 2) == Approx(DownmixMatrix::kFold));  // C -> right
    CHECK(matrix.weight(0, 4) == Approx(DownmixMatrix::kFold));  // Ls -> left
    CHECK(matrix.weight(1, 5) == Approx(DownmixMatrix::kFold));  // Rs -> right

    // Sides stay on their own side.
    CHECK(matrix.weight(1, 0) == Approx(0.0F));
    CHECK(matrix.weight(0, 1) == Approx(0.0F));
    CHECK(matrix.weight(1, 4) == Approx(0.0F));
    CHECK(matrix.weight(0, 5) == Approx(0.0F));
}

TEST_CASE("The LFE is not folded in", "[render][downmix]") {
    // Band-limited rumble meant for a driver the stereo bus does not have.
    // Folding it in adds energy nobody mixed and nothing can reproduce, which
    // is what ATSC A/52 says and what every other tool does by default.
    const DownmixMatrix matrix{6, 2};
    CHECK(matrix.weight(0, 3) == Approx(0.0F));
    CHECK(matrix.weight(1, 3) == Approx(0.0F));

    const DownmixMatrix mono{6, 1};
    CHECK(mono.weight(0, 3) == Approx(0.0F));
}

TEST_CASE("7.1 folds both surround pairs", "[render][downmix]") {
    const DownmixMatrix matrix{8, 2};
    CHECK(matrix.weight(0, 2) == Approx(DownmixMatrix::kFold));  // C
    for (const std::int32_t left : {4, 6}) {
        CHECK(matrix.weight(0, left) == Approx(DownmixMatrix::kFold));
        CHECK(matrix.weight(1, left) == Approx(0.0F));
    }
    for (const std::int32_t right : {5, 7}) {
        CHECK(matrix.weight(1, right) == Approx(DownmixMatrix::kFold));
        CHECK(matrix.weight(0, right) == Approx(0.0F));
    }
}

TEST_CASE("Three channels put the centre in both sides", "[render][downmix]") {
    const DownmixMatrix matrix{3, 2};
    CHECK(matrix.weight(0, 0) == Approx(1.0F));
    CHECK(matrix.weight(1, 1) == Approx(1.0F));
    CHECK(matrix.weight(0, 2) == Approx(DownmixMatrix::kFold));
    CHECK(matrix.weight(1, 2) == Approx(DownmixMatrix::kFold));
}

TEST_CASE("Quad keeps its sides", "[render][downmix]") {
    // L R Ls Rs, with no centre to place.
    const DownmixMatrix matrix{4, 2};
    CHECK(matrix.weight(0, 0) == Approx(1.0F));
    CHECK(matrix.weight(1, 1) == Approx(1.0F));
    CHECK(matrix.weight(0, 2) == Approx(DownmixMatrix::kFold));
    CHECK(matrix.weight(1, 3) == Approx(DownmixMatrix::kFold));
    CHECK(matrix.weight(1, 2) == Approx(0.0F));
}

TEST_CASE("Stereo to mono sums at a half", "[render][downmix]") {
    // Half rather than -3 dB: a mono fold sums correlated material, and
    // equal-power weights on a centred mix come back louder than the stereo it
    // was folded from.
    const DownmixMatrix matrix{2, 1};
    CHECK(matrix.weight(0, 0) == Approx(0.5F));
    CHECK(matrix.weight(0, 1) == Approx(0.5F));
}

TEST_CASE("A layout nobody can name is folded the old way", "[render][downmix]") {
    // Five channels, say. Putting the third one somewhere on the strength of a
    // guess is worse than the honest pass-through, because it cannot be heard
    // as wrong.
    const DownmixMatrix matrix{5, 2};
    CHECK(matrix.isPassThrough());
    CHECK(matrix.weight(0, 0) == Approx(1.0F));
    CHECK(matrix.weight(1, 1) == Approx(1.0F));
    CHECK(matrix.weight(0, 2) == Approx(0.0F));
}

TEST_CASE("Asking outside the matrix answers zero", "[render][downmix]") {
    // So a caller cannot read past a buffer by asking for a channel that is
    // not there.
    const DownmixMatrix matrix{6, 2};
    CHECK(matrix.weight(-1, 0) == Approx(0.0F));
    CHECK(matrix.weight(0, -1) == Approx(0.0F));
    CHECK(matrix.weight(2, 0) == Approx(0.0F));
    CHECK(matrix.weight(0, 6) == Approx(0.0F));
    CHECK(DownmixMatrix{}.weight(0, 0) == Approx(0.0F));
}
